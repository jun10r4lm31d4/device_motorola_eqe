# Arquitetura — HAL de face ArcSoft (eqe)

Este documento registra as decisões tomadas ao substituir o caminho baseado no
APK (`FaceService -> MotoFaceBridge -> FaceAuthService -> ArcSoft`) por um HAL
AIDL nativo, incluindo a captura real de câmera para enrollment/autenticação.

## 1. Por que não usamos mais a `libarcsoft-lib.so`

A `libarcsoft-lib.so` é uma cola JNI fina sobre a `libarcsoft_faceid.so`. Ela
exporta `arcInit(JNIEnv*, tag, path)`, `saveFeature(JNIEnv*, frame)`,
`compare(frame)`, `release()`, `GetRegisterData`/`SaveRegisterData` e três
helpers que chamam Java (`encrypt`, `decrypt`, `isMotoDevice`). Isso trazia
problemas para um daemon vendor:

- assinaturas com `JNIEnv*` (o crash `arcInit+232` vinha de chamar `arcInit` com
  2 argumentos em vez de 3: `x2` virava lixo e caía em `strlen`);
- os helpers Java precisavam ser interpostos por um shim
  (`ArcSoftCompat.cpp`) com `--export-dynamic`;
- estado global escondido (`hHandle`, `sFeatureCount`, `sSavedHanledImages`) que o
  HAL não controlava (ex.: o contador de frames do enroll nunca era zerado, porque
  só `arcSaveFeatureStop`/`arcCompareStart` o zeram e o HAL não os chama).

A `libarcsoft_faceid.so` não usa JNI (só depende de `libmpbase`, `liblog`, `libc`,
`libm`, `libdl`) e exporta 10 funções `ARC_FI_*`. O HAL chama essas funções
diretamente (`ArcSoftApi.h`) e reimplementa, em `ArcSoftFace.cpp`, a sequência
que a cola fazia:

| Cola (antes) | Agora (`ArcSoftEngine`) |
|---|---|
| `arcInit(env, tag, path)` | `ARC_FI_Init(&h,1)` → `SetDeviceUniqueTag(tag)` → `SetRegisterData(templates lidos do arquivo)` → `SetProcessDataType(1)` |
| `compare(frame)` | `DetectFace` → `Recognize` (match = `matched@+0x14`) → `Reset` |
| `saveFeature(env, frame)` | `DetectFace` → `ExtractFeaturesForRegister` → após 21 frames válidos `SetRegisterData` → gravar `facedata.bin` → `Reset` |
| `release()` | `ARC_FI_Uninit(&h)` |
| `GetRegisterData`/`SaveRegisterData` | `parseDb()`/`saveDb()` em `ArcSoftFace.cpp` |
| `encrypt`/`decrypt`/`isMotoDevice` | não existem mais (nada a interpor) |

## 2. Ciclo de vida da engine

Só existe uma engine por processo. `ArcSoftEngine::setUser(userId)`:

1. se o usuário mudou, faz `ARC_FI_Uninit` e descarta os templates em memória;
2. garante o diretório do usuário;
3. lê `facedata.bin` para `mTemplates` e chama `ARC_FI_Init` + tag +
   `SetRegisterData` + `SetProcessDataType`.

A engine é criada na primeira operação da sessão (`beginOp`), não no boot do
daemon. `mTemplates` é a fonte da verdade em memória: `storedFaceCount()` e
`storedFaceIds()` não precisam mais reler o arquivo, e o `sFeatureCount` da cola
(que ficava desatualizado depois de um enroll) deixou de existir.

Contador do enroll: a cola só conclui depois de 21 frames válidos (`ExtractFeatures`
devolveu 0 ou 0x700d). O HAL zera o contador ao iniciar um enroll (frames
separados por mais de 2 s), ao concluir, em `compareFrame` e na reinicialização.

## 3. Persistência sem JNI

Formato de `facedata.bin` (idêntico ao do stock, então bancos já cadastrados
continuam válidos):

```
int32 count
repete count vezes:
    int32 len       # tamanho do template
    int32 id        # id do enrollment (1; vai em ArcRegisterRecord.registId, +0x10)
    int32 encLen    # == len (sem criptografia)
    uint8 payload[encLen]
```

- A gravação é atômica (`facedata.bin.tmp` + `rename`), modo 0600.
- O template fica em texto claro em `/data/vendor_de/<user>/facedata`
  (`system:system 0700`, `face_vendor_data_file`). Modelo equivalente ao dos HALs
  de referência do AOSP.
- Bancos gravados pelo APK stock foram cifrados com chave do Keystore: têm
  `encLen != len`, são rejeitados por `parseDb()` (log de aviso) e a face precisa
  ser cadastrada de novo.
- Se `ARC_FI_SetRegisterData` falhar ao carregar, o HAL registra o erro e segue
  com a engine vazia (o stock também ignorava essa falha); um novo enroll continua
  possível.
- Se a gravação do enroll falhar, a engine é recarregada com o conjunto anterior
  para não divergir do disco.

## 4. Contrato AIDL

- `createSession`: valida `sensorId` e callback; recusa uma segunda sessão com
  `EX_ILLEGAL_STATE`; chama `linkToDeath` no binder do cliente. A morte do
  cliente fecha a sessão, para não travar `createSession` futuros.
- `Session::close()`: idempotente, limpa a operação corrente, desfaz o
  `linkToDeath` e responde `onSessionClosed()`.
- Serialização: uma operação por vez; pedidos concorrentes recebem
  `onError(UNABLE_TO_PROCESS)` (o framework serializa, isto é defesa).
- `revokeChallenge(challenge)`: remove o challenge da lista, mas **sempre**
  responde `onChallengeRevoked` (o AIDL exige notificar até para um challenge
  inexistente). Vários challenges podem ficar em aberto, como o contrato
  permite; a lista é limitada a 8.
- `removeEnrollments`: lê os ids reais da base (`GetRegisterData`), valida e
  trunca a base (`SaveRegisterData(nullptr, nullptr, 0)`); em seguida reinicia a
  engine (`release()` + `arcInit`) porque os templates continuavam carregados
  em memória e uma face removida ainda autenticaria até o próximo boot.
- `getFeatures`/`setFeature`: flags reais persistidas em `features.bin`; sem
  enrollment, devolvem `UNABLE_TO_PROCESS` como o AIDL exige.
- `getAuthenticatorId`/`invalidateAuthenticatorId`: devolvem 0, como o AIDL
  exige para sensores `WEAK`/`CONVENIENCE` (não há vínculo com Keystore).
- HAT não é validado: o sensor é `CONVENIENCE`, sem vínculo com Keystore
  (o AOSP também devolve HAT vazio para esse caso).
- `getEnrollmentConfig` devolve vazio — o default do AOSP faz o mesmo.
- `detectInteraction` responde `UNABLE_TO_PROCESS`, como o AIDL manda quando
  `supportsDetectInteraction == false`.
- `enroll`/`authenticate` capturam frames reais e chamam a engine ArcSoft
  (ver seção 5).

## 5. Pipeline de câmera (implementado)

O AIDL de face não recebe frames: o HAL precisa capturar da câmera. A solução
usa o **NDK camera2 na variante vendor** (`libcamera2ndk_vendor.so`), que fala
com `cameraserver` pelo AIDL `android.frameworks.cameraservice.service`, e o
`AImageReader` do `libmediandk`. Isso evita reimplementar o protocolo do
provider de câmera (AIDL/HIDL) dentro do HAL. `libcamera2ndk` (variante system)
não é vendor-available; por isso o sufixo `_vendor`.

Fluxo por operação:

1. `Session::startEnroll()`/`authenticate()` chamam `beginOp()` e iniciam um
   worker thread (`Session::runCapture`), para que os callbacks binder não
   bloqueiem a thread do cliente.
2. `CameraCapture::start()` enumera câmeras com `ACameraManager`, ordena
   `LENS_FACING_FRONT` primeiro e depois `LENS_FACING_BACK`, abre a primeira com
   `ACameraManager_openCamera` e configura um `AImageReader` 640x480
   `AIMAGE_FORMAT_YUV_420_888` (o mesmo formato/tamanho do
   `Camera2Helper` stock), com template `TEMPLATE_PREVIEW` e AF contínuo.
3. O callback `onImageAvailable` converte cada `AImage` para **NV21** (plane 0 =
   Y; plane 1 = U/Cb; plane 2 = V/Cr, intercalados V/U a cada 2x2) e guarda o
   frame mais recente. `waitForFrame()` é o ponto de sincronização com o worker.
4. Enrollment: `ArcSoftEngine::enrollFrame()` (Detect → ExtractFeaturesForRegister → persistência). Código 0 gera
   `EnrollmentFrame(ENROLLMENT_FINISHED)` + `onEnrollmentProgress(id, 0)`;
   outros códigos viram `onEnrollmentFrame` com `AcquiredInfo` mapeado dos
   códigos `ARC_FI_*` (`ArcConstant.smali`).
5. Autenticação: `ArcSoftEngine::compareFrame()` (Detect → Recognize). Código 0 gera
   `onAuthenticationSucceededWithResult` (V5) ou o fallback deprecado
   `onAuthenticationSucceeded`; `0x7006` gera `onAuthenticationFailed` no
   máximo a cada 500 ms; os demais geram `onAuthenticationFrame`.
6. O HAL usa sempre a primeira câmera frontal enumerada. Não existe opção de
   troca/fallback: todas as outras câmeras do dispositivo são traseiras. A
   primeira câmera frontal é a única listada em `mCameraIds`, e nenhuma troca é
   feita durante a captura — isso fechava o device enquanto o framework
   cancelava a operação (outro biométrico vencia a corrida), travava o teardown
   da câmera e deixava o próximo `authenticate()` preso em
   `stopWorker()/join()`.
7. Cancelamento/timeout: `mCancelRequested` é atômico; `close()` e
   `cancelOperation()` o setam e fazem join do worker. A geração implícita é o
   próprio worker: ao sair, ele destrói o `CameraCapture`, fecha o device e
   descarta frames pendentes. `detectInteraction` continua retornando
   `UNABLE_TO_PROCESS`. Uma falha transitória de `camera.start()` reporta
   `UNABLE_TO_PROCESS` em vez de `HW_UNAVAILABLE`, para que o framework não
   descarte a sessão e tente recriá-la enquanto a antiga ainda está viva.
8. `createSession()` reaproveita a sessão ativa quando o usuário é o mesmo.
   Devolver `EX_ILLEGAL_STATE` nesse caminho fazia `FaceStartUserClient` lançar
   `IllegalStateException` e matar o `system_server`.

`SensorProps` agora reporta `halControlsPreview = true` e
`enrollPreviewWidth/Height = 640/480`, e o RRO do Settings define
`config_face_enroll_hal_owns_camera = true` com `face_preview_width/height`
iguais. Isso impede que o Settings abra um segundo cliente de câmera no mesmo
sensor. O HAL recebe o preview como `android.view.Surface` (AIDL
`NativeWindow`), mantém uma referência ao `ANativeWindow` e o anexa como
segundo output da sessão de captura, então a área de enroll mostra a imagem da
câmera. Os frames continuam sendo reportados por
`onEnrollmentFrame`/`onAuthenticationFrame` para guiar o usuário.

## 6. Itens de build/sepolicy

- Só `libarcsoft_faceid.so` e `libmpbase.so` são pré-built em
  `vendor/motorola/eqe/Android.bp` (origem em `proprietary/vendor/lib64`, para
  onde `proprietary-files.txt` as extrai do APK stock), com `soc_specific:
  true`, instaladas em `/vendor/lib64`. Remova de lá os módulos
  `libarcsoft-lib` e `libarcsoftbase` e as linhas correspondentes do
  `proprietary-files.txt` e do `PRODUCT_PACKAGES`.
- `/data/vendor_de/<user>/facedata` já tem label e permissões; nenhuma regra
  nova é necessária.
- O daemon usa o domínio `hal_face_default` (de
  `system/sepolicy/vendor/hal_face_default.te`), não um domínio novo: ele já
  traz `hal_server_domain(hal_face_default, hal_face)`, `init_daemon_domain` e
  o `exec_type` do binário. As regras específicas do HAL ficam em
  `aidl/face/sepolicy/hal_face_camera.te`, e esse diretório foi adicionado a
  `BOARD_VENDOR_SEPOLICY_DIRS` no `BoardConfig.mk` (regras `.te` não podem ser
  declaradas em `Android.bp`).
- Câmera: um processo vendor **não pode** abrir `/dev/video*` (há um
  `neverallow` em `system/sepolicy/private/hal_camera.te` para
  `{ halserverdomain -hal_camera_server }`). O HAL usa o `cameraserver` via
  `libcamera2ndk_vendor`. O `.te` faz:
  - `binder_use(hal_face_default)` e `binder_call(hal_face_default,
    cameraserver)` para a conexão AIDL;
  - `allow hal_face_default cameraserver_service:service_manager find;` e
    `permission_checker_service:service_manager find`;
  - `hal_client_domain(hal_face_default, hal_graphics_allocator)` para os
    buffers do `AImageReader`;
  - `allow ... dmabuf_system_heap_device:chr_file r_file_perms` para mapear os
    buffers (mesma regra do `hal_sensors_default`).
- Permissão de câmera: o HAL roda como `system` (`uid 1000`). O SystemUI
  compartilha `android.uid.system` e declara `android.permission.CAMERA`; o
  `DefaultPermissionGrantPolicy` concede aos componentes do sistema todas as
  permissões runtime que eles solicitam. Portanto o uid já tem `CAMERA` e o
  `CameraService` aceita `ACameraManager_openCamera` sem appops adicionais.
- `sepolicy/vendor/hal_moto_face.te` foi removido: o `file_contexts` rotula o
  binário como `hal_face_default_exec` e o domínio `hal_face_default` já existe
  em `system/sepolicy/vendor/hal_face_default.te`. O `.te` antigo nunca era
  usado.
- O `.rc` não é mais `oneshot`: se o daemon morrer, o init reinicia o serviço
  (útil durante o bring-up). Contém a linha `interface aidl ...` padrão.

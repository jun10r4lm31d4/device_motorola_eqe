# Face unlock HAL AIDL — implementação Motorola/ArcSoft

Esta pasta contém o daemon HAL nativo que implementa o AIDL **padrão do AOSP**
`android.hardware.biometrics.face` (V5), sem duplicar a interface localmente.
O `aidl_interface` oficial continua em `hardware/interfaces/biometrics/face/aidl`;
aqui apenas linkamos `android.hardware.biometrics.face-V5-ndk`.

## Conteúdo

- `Android.bp` — declara o binário
  `android.hardware.biometrics.face-service.motorola`.
- `main.cpp` — registra `IFace/default` no `servicemanager`.
- `Face.cpp` — `getSensorProps()` e `createSession()` (validação de sensor,
  sessão única, `linkToDeath`).
- `Session.cpp` — implementação de `ISession` (challenge, enumerate, remove,
  features, authenticatorId, cancelamento, `close()`).
- `ArcSoftFace.cpp` — engine ArcSoft (singleton por processo, reinicializado
  por usuário): chama `ARC_FI_*` direto e lê/grava `facedata.bin` em
  `/data/vendor_de/<user>/facedata`.
- `ArcSoftApi.h` — API C da `libarcsoft_faceid.so` recuperada do disassembly
  (substitui o antigo `ArcSoftCompat.h`; o shim `ArcSoftCompat.cpp` foi removido).
- `CameraCapture.h/.cpp` — cliente native camera2 (NDK vendor) com
  `AImageReader` 640x480 `YUV_420_888` convertido para NV21.
- `android.hardware.biometrics.face.motorola.xml` — fragmento VINTF para a
  interface padrão (`android.hardware.biometrics.face` V5, instância `default`).
- `android.hardware.biometrics.face-service.motorola.rc` — serviço init.
- `sepolicy/hal_face_camera.te` — regras SELinux para `libcamera2ndk_vendor`
  acessar o `cameraserver` e o graphics allocator; o diretório está em
  `BOARD_VENDOR_SEPOLICY_DIRS` no `BoardConfig.mk`.

## Fluxo

```
FaceService -> android.hardware.biometrics.face-service.motorola
                 |-> libcamera2ndk_vendor.so -> cameraserver (AIDL)
                 |-> libmediandk.so          (AImageReader YUV_420_888)
                 |-> libarcsoft_faceid.so   (ARC_FI_*, chamada direta)
                 `-> libmpbase.so           (MMem*, dependência da anterior)
```

Nenhuma dependência de APK/Java: o APK `MotoFaceUnlockArcSoft` deixou de fazer
parte do caminho. As bibliotecas ArcSoft usadas (`libarcsoft_faceid.so` e `libmpbase.so`) saíram do APK e são instaladas em
`/vendor/lib64` (módulos pré-built com `soc_specific: true` em
`vendor/motorola/eqe/Android.bp`),
porque um processo vendor não enxerga `/product/lib64`.

## Estado atual (o que funciona)

| Operação | Estado |
|---|---|
| `getSensorProps` | OK (`CONVENIENCE`, RGB, 1 enrollment, `halControlsPreview=true`, preview 640x480) |
| `createSession` | OK (valida `sensorId`, sessão única, `linkToDeath`) |
| `generateChallenge` / `revokeChallenge` | OK (challenges de 64 bits; `revoke` sempre notifica, como o AIDL exige) |
| `enumerateEnrollments` | OK (lê os ids reais da base via `GetRegisterData`) |
| `removeEnrollments` | OK (trunca a base via `SaveRegisterData` e reinicia a engine) |
| `getFeatures` / `setFeature` | OK (flags persistidas em `features.bin`) |
| `getAuthenticatorId` / `invalidateAuthenticatorId` | OK (0: sensor `CONVENIENCE`, conforme o AIDL) |
| `authenticate` | OK: captura NV21 real, `compareFrame()` e callbacks de sucesso/falha |
| `enroll` | OK: captura NV21 real, `enrollFrame()` e `onEnrollmentProgress` |
| `detectInteraction` | não suportado (`supportsDetectInteraction=false`) |

A captura usa `libcamera2ndk_vendor` + `AImageReader`. O HAL usa sempre a
primeira câmera frontal; não há troca para as câmeras traseiras. A superfície de
preview recebida do framework (`enrollWithOptions`)
é anexada à sessão de câmera do HAL, então o enrollment mostra a imagem da
câmera. `halControlsPreview=true` impede que o Settings abra uma segunda câmera;
os frames também são devolvidos por `onEnrollmentFrame`/
`onAuthenticationFrame` para o UI guiar o usuário. Detalhes em
`ARCHITECTURE.md`, seção 5.

## Persistência

Os templates ficam em:

```
/data/vendor_de/<userId>/facedata/facedata.bin
/data/vendor_de/<userId>/facedata/features.bin          # bitmask de features
```

`vold_prepare_subdirs.cpp` cria o diretório como `system:system 0700` e o
SELinux o rotula `face_vendor_data_file`. `system/sepolicy/private/hal_face.te`
já concede `create_file_perms`/`rw_dir_perms` para `{hal_face -coredomain}`, o
que inclui `hal_face_default`; **não é necessário sepolicy novo**.

O formato do arquivo continua o da `libarcsoft-lib.so`
(`count`, e por template `len`, `id`, `encLen`, `payload`; `encLen == len`), então
o `facedata.bin` já cadastrado segue válido. A criptografia do stock dependia de
`JNIEnv` (classe Java `MotoUnlockEncryptor`); aqui o HAL lê e grava o arquivo ele
mesmo, e o armazenamento é em texto claro, protegido apenas por SELinux/permissões, como
nos HALs de referência do AOSP. Detalhes e limitações em `ARCHITECTURE.md`.

## SELinux

`sepolicy/hal_face_camera.te` (neste diretório, incluído por
`BOARD_VENDOR_SEPOLICY_DIRS`) permite que `hal_face_default` encontre o
`cameraserver`, faça binder calls e use o graphics allocator para os buffers do
`AImageReader`. Abrir `/dev/video*` diretamente é proibido por `neverallow`.
O binário continua rotulado como `hal_face_default_exec` por
`sepolicy/vendor/file_contexts`.

## Build

```bash
m android.hardware.biometrics.face-service.motorola
m selinux_policy    # valida as regras .te e os neverallows
```

O binário é instalado em
`/vendor/bin/hw/android.hardware.biometrics.face-service.motorola`.

## Verificação no device

```bash
# serviço registrado?
adb shell service list | grep -i face

# bibliotecas no lugar certo (vendor, não product)
adb shell ls -l /vendor/lib64/libarcsoft_faceid.so /vendor/lib64/libmpbase.so

# o binário não deve mais depender da cola JNI
adb shell readelf -d /vendor/bin/hw/android.hardware.biometrics.face-service.motorola | grep NEEDED

# banco do usuário 0
adb shell ls -lZ /data/vendor_de/0/facedata

# logs
adb logcat -s face-moto face-moto-arcsoft face-moto-session face-moto-camera
```

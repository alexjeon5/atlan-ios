# 기기 테스트 가이드

CI 가 만든 서명 없는 IPA 를 iPhone/iPad 에 설치해서 앱 셸이 어디까지 가는지 확인하는 방법이야. 원본 앱·라이브러리·지도 파일은 저장소에 없고, 각자 가진 파일을 직접 넣어야 해.

## 준비물

- iOS/iPadOS 15 이상 기기, 가로 모드 전용 앱
- iloader 같은 사이드로딩 도구 (IPA 서명·설치용)
- 본인이 가진 아틀란 데이터 폴더 `ATLAN` (안에 `APP/atlan.dat` 이 있으면 따로 준비할 게 없어)

## 1. IPA 받기

1. GitHub 저장소 → Actions → 가장 최근 `main` 의 `Runtime` 워크플로 run 을 연다 (초록 체크인지 확인).
2. 아래 Artifacts 에서 `atlan-runtime-ios-unsigned` 를 받는다. zip 안에 `.ipa` 가 들어 있어.
3. iloader 로 서명하고 기기에 설치한다. 신뢰 설정(설정 → 일반 → VPN 및 기기 관리)이 필요할 수 있어.

## 2. 앱 패키지

- 앱은 Documents 의 `ATLAN/APP/atlan.dat` 을 그대로 실행해. 안드로이드 아틀란이 데이터 폴더에 넣어 두는 APK 사본이라, `ATLAN` 폴더를 통째로 넣으면 끝이야. APK 파일(`*.apk`)을 `ATLAN/APP` 나 `ATLAN` 에 넣어도 돼. 여러 개면 수정 시각이 가장 새로운 걸 써.
- 앱을 열면 패키지를 찾아 자동으로 실행해. 로그 첫 줄들이 `ATLAN/APP/atlan.dat`, `package lib/armeabi/libatlan3dv5.so version 1.8.2.35` 처럼 나오면 패키지에서 라이브러리를 꺼낸 거야.
- 업데이트는 새 `atlan.dat` 이나 APK 로 바꿔 넣고 앱을 다시 열면 돼. 꺼낸 라이브러리를 따로 저장하지 않아서 예전 버전이 남지 않아.
- 패키지가 없을 때만 예전 방식이 쓰여: 왼쪽 위 첫 번째 버튼으로 `libatlan3dv5.so` 를 고르면 `Application Support/guest.so` 로 복사해 둬.

## 3. 지도·데이터 넣기

- 파일 앱 → **내 iPhone > (앱 이름)** 폴더 = 앱의 Documents 폴더야. 여기에 `ATLAN` 폴더를 통째로 복사해 넣어. 안에 `APP`, `MAP`, `UI`, `Sys` 같은 폴더가 있어야 해.
- 앱은 `system.ini` 에 적힌 안드로이드 경로를 Documents 의 `ATLAN` 폴더로 연결해 줘. `ROOTPATH` 에 적힌 경로가 무엇이든 그게 `ATLAN` 폴더고, `/storage/emulated/0/ATLAN/…`, `/storage/sdcard1/Atlan/…` 처럼 `atlan` 이라는 폴더(대소문자 무관)를 지나는 다른 절대 경로도 같은 폴더로 가. 기본 ini 의 `/data/app_storage/Atlan/…` 만 `ATLAN/NAND/…` 로 가. 그래서 ini 의 경로 값은 기기에 맞게 고칠 필요가 없어.
- 화면은 ini 의 `SCREENWIDTH`×`SCREENHEIGHT` 크기로 그려서 기기 화면 전체에 늘려 보여 줘(안드로이드 앱과 같은 동작). 앱 UI 가 800×480 배치로 고정돼 있어서 가로만 넓히면 UI 가 왼쪽에 몰려. ini 의 `FIXEDSCREENWIDTH`·`FIXEDSCREENHEIGHT` 값과는 상관없이 항상 그렇게 해. 화면은 초당 최대 20프레임으로 그려.
- 같은 내용이 반복되는 로그(없는 파일을 계속 여는 줄, 앱의 반복 콜백)는 처음 한 번만 남겨.
- 지도 파일 용량이 크니까(수 GB) 복사 시간과 기기 저장 공간을 확인해줘.
- 데이터가 없어도 실행은 끝까지 가고, 없는 파일은 `fopen … failed` 로그로 남아.
- 데이터를 바꾸려면 같은 폴더의 파일을 바꿔 넣으면 돼.

## 4. 실행

패키지가 있으면 앱을 열 때 자동으로 시작해. 다시 돌리려면 두 번째 버튼(▶︎)을 누른다. 게스트는 전용 스레드에서 돌고 로그는 화면 아래 텍스트 영역에 쌓여.

## 지금 기대하는 로그

앱을 켜면 먼저 `selftest <숫자>` 가 나와. 실행하면 대략 이런 순서야 (서버에서 같은 코드를 돌린 결과 기준이야).

```
ATLAN/APP/atlan.dat
package lib/armeabi/libatlan3dv5.so version 1.8.2.35
library sha256=<64자리 16진수>
loaded mapped_bytes=16486400 exports=25963 imports_trapped=262 initializers=415
compat imports trapped=262 missing=0
initializers_completed=415
android_log 4 ATLAN30_LOG: JNI_OnLoad called
android_log 4 ATLAN30_LOG: JNI_OnLoad called 2
android_log 4 ATLAN30_LOG: JNI_OnLoad: registerNatives function is success
jni class ... / jni native ... (여러 줄)
JNI_OnLoad returned 0x10004
compat entries=... missing=0
android_log 3 atlan MainActivity: nativeInit()
opensles ... / fopen ... / property ... (여러 줄)
java MainActivity.setSurfaces(800,480)
nativeInit returned 0x0
android_log 4 atlan AtlanUISurfaceView: AtlanUISurfaceView createUISF()
Java_com_atlan_AtlanUISurfaceView_createUISF returned 0x0
android_log 4 ATLAN30_LOG: AtlanGLSurfaceView createGLSF()
(지도 파일을 여는 fopen 줄이 많이 나오고, 오래 걸려)
Java_com_atlan_AtlanGLSurfaceView_createGLSF returned 0x0
android_log 4 atlan AtlanUISurfaceView: AtlanUISurfaceView changedUISF()
Java_com_atlan_AtlanUISurfaceView_changedUISF returned 0x0
java AtlanGLThread.callBackGLThread()
Java_com_atlan_AtlanGLSurfaceView_renderGLSF returned 0x0   (3번 반복)
pump stop=0 retired=... live_threads=20
audio callbacks=0 players=1
```

- 정상 종료 지점은 `audio callbacks=… players=1` 줄이야. 이 줄까지 나오면 이번 단계는 성공이고, 그 뒤에 아무것도 안 뜨는 게 **현재 예상 결과**야. (지도 데이터가 없으면 `createGLSF` 가 빨리 끝나고 `players=0` 이야.)
- `compat` 줄은 앱 업데이트 대응(M10) 점검이야. 런타임에 없는 import는 `compat missing_import 이름`(데이터면 `missing_data`), 어댑터가 부르는 진입점이나 입력 바인딩의 네이티브가 이미지에 없으면 `compat missing_entry`/`compat missing_native`, 게스트가 찾았지만 호스트가 답하지 않는 Java 메서드는 `compat jni_unhandled`로 나와. 모두 실행 전에(또는 `JNI_OnLoad`·init 직후에) 목록으로 나오고, 실제로 불리면 그 이름으로 멈춰(`missing_service=`, `missing_data=`, `no host implementation for`). 검증된 빌드는 `missing=0`이 정상이야.
- 핵심 확인 줄은 `JNI_OnLoad returned 0x10004`, `nativeInit returned 0x0`, 그리고 `createGLSF returned 0x0`(지도 데이터가 있을 때) 이야.
- `createGLSF` 는 지도 파일을 읽느라 폰에서 몇 분 걸릴 수 있어. 서버(arm64 리눅스)에서는 약 3분이야. 그동안 앱이 멈춘 것처럼 보여도 로그가 늘어나면 정상이야.
- `guest stdout read error: Bad file descriptor` 줄이 많이 반복되는 건 앱 자신의 동작이야. system.ini 의 `CLOUDMAPPATH=/map/CLOUD_MAP` 은 클라우드 지도 저장 폴더인데, 쓸 수 있는 폴더가 없으면 앱이 `open` 실패 값 -1 을 확인하지 않고 그대로 `read` 해서 `perror` 가 찍혀. 이 줄은 해롭지 않아. 실제 안드로이드 기기에서 `/map` 이 없을 때도 똑같이 나와. 일부러 이 폴더를 만들어 주지 않아: 만들어 주면 앱이 클라우드 지도 받기 스레드를 살리는데, 앱 안에서 그 스레드 초기화와 다른 스레드의 첫 요청이 경합해서 세 번째 `renderGLSF` 부근에서 널 접근으로 멈춰. 클라우드 받기에는 소켓도 필요해서 지금은 쓸모가 없어. 그래서 ini 에 `CLOUDMAPPATH` 가 없거나 다른 값으로 바뀌어 있어도 앱에는 항상 `CLOUDMAPPATH=/map/CLOUD_MAP` 인 사본을 읽혀 줘.
- 지도·데이터를 안 넣었으면 `fopen ATLAN/MAP/WAVE/….idx … failed` 가 찍혀. 그래도 `nativeInit returned 0x0` 까지는 가. 데이터를 넣으면 `ok` 로 바뀌는지, 그 뒤에 새로 멈추는 곳이 있는지 보는 게 이번 테스트의 목적이야.
- `setSurfaces` 는 화면 연결이 아직 없어서 호출만 기록하고 아무 일도 안 해. 화면에는 아직 아무것도 그려지지 않아.
- 화면에는 아직 아무것도 그려지지 않아 (그래픽 백엔드 연결 전). 시작음 같은 소리가 날 수 있어.

## 실패했을 때 로그의 의미

| 로그 | 의미 |
| --- | --- |
| `library missing` | `ATLAN/APP/atlan.dat`·APK 도, 가져온 `.so` 도 없음 |
| `package …` 줄 없이 끝나는 오류(`not an APK …`, `… fails its CRC check`, `package has no 32-bit ARM library …`) | 패키지가 깨졌거나 32비트 ARM 라이브러리가 없음 |
| `library …` 없이 `import failed: …` | 파일 가져오기 실패 |
| `load failed: …` | ELF 로드 실패 (잘못된 파일, ARM32 가 아님) |
| `initializer[N] …` 로 시작하는 줄 | N 번째 생성자에서 멈춤 (뒤에 이유가 붙어) |
| `JNI_OnLoad …` 로 시작하는 실패 줄 | JNI 초기화 중 멈춤 |
| `nativeInit …` 로 시작하는 실패 줄 | `nativeInit` 중 멈춤 (없는 서비스 이름이나 예산 초과가 붙어) |
| `pump stopped at missing service …` | 게스트 스레드가 아직 없는 서비스를 호출 |
| `audio callback: …` | 오디오 콜백 실행 중 오류 |

## 리포트할 때

- 로그 영역 **전체 스크린샷** (길면 위쪽과 아래쪽을 나눠서). 마지막 줄이 보이게 해줘.
- 가장 중요한 건 마지막 몇 줄과 `nativeInit …` 줄이야. `fopen … failed` 로 시작하는 줄이 많으면 그 경로 목록도 같이 보여줘.
- 기기 모델, iOS 버전, 설치에 쓴 도구.
- 앱이 죽었다면(튕김) 어느 시점에서 죽었는지, 그리고 설정 → 개인정보 보호 및 보안 → 분석 및 향상 → 분석 데이터에서 `AtlanRuntime` 크래시 로그가 있으면 같이 보내줘.
- 멈췄다면 얼마나 기다렸는지, 기기가 뜨거워졌는지.

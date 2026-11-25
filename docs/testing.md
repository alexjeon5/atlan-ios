# 기기 테스트 가이드

CI 가 만든 서명 없는 IPA 를 iPhone/iPad 에 설치해서 앱 셸이 어디까지 가는지 확인하는 방법이야. 원본 앱·라이브러리·지도 파일은 저장소에 없고, 각자 가진 파일을 직접 넣어야 해.

## 준비물

- iOS/iPadOS 15 이상 기기, 가로 모드 전용 앱
- iloader 같은 사이드로딩 도구 (IPA 서명·설치용)
- 본인이 가진 ARM32 네이티브 라이브러리 `libatlan3dv5.so`
- 본인이 가진 지도 데이터 폴더/파일

## 1. IPA 받기

1. GitHub 저장소 → Actions → 가장 최근 `main` 의 `Runtime` 워크플로 run 을 연다 (초록 체크인지 확인).
2. 아래 Artifacts 에서 `atlan-runtime-ios-unsigned` 를 받는다. zip 안에 `.ipa` 가 들어 있어.
3. iloader 로 서명하고 기기에 설치한다. 신뢰 설정(설정 → 일반 → VPN 및 기기 관리)이 필요할 수 있어.

## 2. 라이브러리 가져오기

1. 기기의 파일 앱에 `libatlan3dv5.so` 를 넣어 둔다 (iCloud Drive 든 "내 iPhone" 이든 상관없어).
2. 앱을 열고 왼쪽 위 첫 번째 버튼(아래 화살표 아이콘)을 눌러 파일 선택기에서 그 `.so` 를 고른다.
3. 로그에 `library libatlan3dv5.so` 가 뜨면 성공. 앱이 `Application Support/guest.so` 로 복사해 두니까 다시 안 골라도 돼.

## 3. 지도·데이터 넣기

- 파일 앱 → **내 iPhone > (앱 이름)** 폴더 = 앱의 Documents 폴더야. 여기에 `ATLAN` 폴더를 통째로 복사해 넣어. 안에 `APP`, `MAP`, `UI`, `Sys` 같은 폴더가 있어야 해.
- 앱은 `system.ini` 에 적힌 안드로이드 경로(`/storage/sdcard1/Atlan/…`)를 Documents 의 `ATLAN/…` 로, 앱이 쓰는 저장 공간(`/data/app_storage/Atlan/…`)은 `ATLAN/NAND/…` 로 연결해 줘. 그래서 `ATLAN/NAND` 는 앱이 직접 만드는 폴더야.
- 지도 파일 용량이 크니까(수 GB) 복사 시간과 기기 저장 공간을 확인해줘.
- 데이터가 없어도 실행은 끝까지 가고, 없는 파일은 `fopen … failed` 로그로 남아.
- 데이터를 바꾸려면 같은 폴더의 파일을 바꿔 넣으면 돼.

## 4. 실행

두 번째 버튼(▶︎)을 누른다. 게스트는 전용 스레드에서 돌고 로그는 화면 아래 텍스트 영역에 쌓여.

## 지금 기대하는 로그

앱을 켜면 먼저 `selftest <숫자>` 가 나와. 실행하면 대략 이런 순서야 (서버에서 같은 코드를 돌린 결과 기준이야).

```
loaded mapped_bytes=16486400 exports=25963 imports_trapped=262 initializers=415
initializers_completed=415
android_log 4 ATLAN30_LOG: JNI_OnLoad called
android_log 4 ATLAN30_LOG: JNI_OnLoad called 2
android_log 4 ATLAN30_LOG: JNI_OnLoad: registerNatives function is success
jni class ... / jni native ... (여러 줄)
JNI_OnLoad returned 0x10004
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
- 핵심 확인 줄은 `JNI_OnLoad returned 0x10004`, `nativeInit returned 0x0`, 그리고 `createGLSF returned 0x0`(지도 데이터가 있을 때) 이야.
- `createGLSF` 는 지도 파일을 읽느라 폰에서 몇 분 걸릴 수 있어. 서버(arm64 리눅스)에서는 약 3분이야. 그동안 앱이 멈춘 것처럼 보여도 로그가 늘어나면 정상이야.
- 예전에 반복되던 `guest stdout read error: Bad file descriptor` 는 원인을 고쳤어 (system.ini 의 `CLOUDMAPPATH=/map/CLOUD_MAP` 폴더가 없어서 앱이 `open(O_CREAT)` 실패 값 -1 을 그대로 `read` 했던 거야). 이제 `ATLAN/NAND/CLOUD_MAP` 으로 연결돼. 지도 데이터가 있으면 그 뒤 세 번째 `renderGLSF` 에서 `CMapReadThread::enqueue` 가 널 객체로 불려 `memory_fault address=0x15c` 로 멈춰. 원인 조사 결과: `InitializeMapDownload`(0x3b5f30)는 성공하지만(스레드 둘 다 생성됨) 전역 포인터(0x10eec71c)를 맨 끝, `usleep(50ms)` 뒤에야 채워. 그 50ms 동안 앱의 `MainProc` 스레드(GPS → `CMgrMap::GetAddressCode` → `CMneCloudMapFile::LoadTile`)가 먼저 `enqueue` 를 불러서 널을 읽어. 런타임 스텁 문제가 아니라 스레드 실행 순서 문제로 보여 (아직 해결 못 함).
- 지도·데이터를 안 넣었으면 `fopen ATLAN/MAP/WAVE/….idx … failed` 가 찍혀. 그래도 `nativeInit returned 0x0` 까지는 가. 데이터를 넣으면 `ok` 로 바뀌는지, 그 뒤에 새로 멈추는 곳이 있는지 보는 게 이번 테스트의 목적이야.
- `setSurfaces` 는 화면 연결이 아직 없어서 호출만 기록하고 아무 일도 안 해. 화면에는 아직 아무것도 그려지지 않아.
- 화면에는 아직 아무것도 그려지지 않아 (그래픽 백엔드 연결 전). 시작음 같은 소리가 날 수 있어.

## 실패했을 때 로그의 의미

| 로그 | 의미 |
| --- | --- |
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

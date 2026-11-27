# iOS PoC

macOS와 Xcode에서 네이티브 UIKit 앱을 생성한다. 앱은 `atlan_runtime`을 정적으로 링크하고 합성 ARM32 계산 결과를 표시한다. 원본 앱을 포함하거나 실행하지 않는다.

```sh
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DATLAN_BUILD_IOS=ON -DBUILD_TESTING=OFF
cmake --build build-ios --config Debug -- CODE_SIGNING_ALLOWED=NO
```

서명 없는 컴파일 확인용 명령이다. 기기 설치와 IPA 내보내기는 Xcode에서 개발팀·bundle identifier·프로비저닝을 설정한 뒤 수행해야 한다. 개인 서명 설정은 저장소에 넣지 않는다. JIT entitlement는 요구하지 않는다.

Linux ARM64에서 CPU 코어를 실행한 결과는 iOS 기기 검증을 대체하지 않는다. CI는 Apple SDK로 앱을 컴파일하지만 UIKit 표시·기기 수명주기·실기기 메모리 사용량 검증은 별도로 필요하다.

## 앱 셸

앱은 `atlan_runtime`을 정적 링크하며 상단에 가져오기·실행 버튼과 지도 상태, 아래에 진단 로그를 보여준다. 게스트는 전용 pthread(8MB 스택)에서 실행하고 로그는 메인 스레드로 넘긴다.

- 앱 패키지: Documents의 `ATLAN/APP/atlan.dat`(안드로이드 앱이 데이터 폴더에 두는 APK 사본, 12바이트 헤더와 4바이트 XOR 키), `ATLAN/APP/*.apk`, `ATLAN/*.apk` 중 수정 시각이 가장 새로운 패키지를 찾아(폴더·파일 이름 대소문자 무관, `adapters/atlan/app_package`) 실행할 때마다 그 안의 ARM32 라이브러리(`lib/armeabi-v7a`, `lib/armeabi`의 `libatlan*.so` 우선)를 메모리로 꺼내 실행한다. 압축 해제는 자체 DEFLATE 디코더로 하고 CRC를 검사한다. 앱 업데이트는 새 `atlan.dat`이나 APK를 같은 폴더에 넣는 것이며, 꺼낸 라이브러리를 따로 저장하지 않으므로 이전 버전이 남지 않는다. 패키지가 있으면 앱을 열 때(또는 패키지를 넣고 앱으로 돌아올 때) 한 번 자동으로 실행한다. 버전은 APK의 `assets/version.txt`(없으면 .dat 헤더)로 `package … version …` 줄에 남는다.
- 네이티브 라이브러리: 패키지가 없을 때만 쓴다. 파일 앱 선택기로 가져와 `Application Support/guest.so`로 교체 저장한다(수십 MB 규모라 복사). 앱과 IPA에 번들하지 않는다.
- 지도: 복사하지 않는다. 사용자가 파일 앱·Finder로 앱 Documents 폴더에 직접 넣거나 옮기고(`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`), Documents에서 가장 큰 일반 파일을 그 자리에서 사용한다. 교체는 같은 폴더의 파일을 바꿔 넣는 것이다. 앱은 크기와 앞·뒤 4KB 읽기 가능 여부만 검증하고(`engine/shell/data_file`), 형식별 헤더 검증은 형식이 정해지면 `magic`으로 추가한다. 잘못된 파일은 사유와 함께 거부하며 보관·롤백하지 않는다.
- 실행: 해석기가 `diagnostic_traps`로 로드하고 libc·문자열·printf·힙·mmap·스레드·mutex·조건변수·세마포어·가상 파일·스레드 키·android_log·OpenSL ES(Apple AudioQueue 싱크) 서비스를 등록한 뒤 생성자를 차례로 실행하고, `JNI_OnLoad`가 있으면 최소 JavaVM/JNIEnv로 호출하고, `MainActivity.nativeInit`을 실행하며(Documents를 파일 루트로 사용, `setSurfaces`는 호출만 기록), 이어서 게스트 스레드와 오디오 콜백을 한 번 `pump`한다. 테스트 절차는 `docs/testing.md`. 서비스 구성은 `tools/elf.cpp`와 같아야 한다. 중지는 생성자 경계에서 협력적으로 처리된다(호출당 예산 3,000,000 명령).
- 지도를 게스트 파일 접근에 연결하는 가상 파일 디스크립터는 M4b 범위이며 아직 없다.
- 터치와 위치(CoreLocation)는 입력 큐를 거쳐 게스트 네이티브 메서드로 전달한다. 바인딩은 Documents의 `input.cfg`로 주입한다([입력 연결](input.md)). 이 파일이 없으면 앱은 실행되지만 터치와 위치가 게스트에 전달되지 않는다(로그 `input bindings missing`, `input: no binding for …`). 런처 설정의 개발자 탭에서 위치 고정을 켜면 CoreLocation 대신 지정한 좌표·진행 방향을 속도 0인 위치 값으로 1초마다 보낸다(로그 `location pinned …`). 내비게이션 실행 중에도 바로 바뀌며, 좌표 칸은 `위도, 경도` 외에 지도 앱이 복사하는 `37.5° N, 127.0° E` 형식도 받는다(`engine/shell/coordinate.hpp`). 첫 프레임들과 `pump` 뒤에는 중지될 때까지 프레임을 계속 그린다.
- 네트워크: 게스트 소켓은 호스트 소켓으로, Java 쪽 HTTP 요청은 평문 소켓(http)과 NSURLSession(https)으로 처리하고, Network 프레임워크의 연결 상태를 게스트의 온라인 상태로 알린다. 연결과 요청은 Documents의 `net.log`에 남는다([네트워크](network.md)).

CI는 `Release-iphoneos` 앱을 `Payload/`로 압축한 서명 없는 IPA(`atlan-runtime-ios-unsigned`)를 아티팩트로 올린다. 사이드로딩 도구(iloader 등)로 서명·설치한다.

공개 빌드에는 앱 아이콘이 없다. 아이콘은 원본 앱 자산에서 나온 것이라 저장소 밖(`private/`)에 두고, 서명 전에 `tools/inject_app_icon.py IN.ipa ICON`으로 IPA에 넣는다. ICON은 크기별 PNG 폴더(파일 이름은 스크립트의 `ICON_FILES`)나 정사각 PNG 한 장(Pillow로 축소)이다. 아이콘은 에셋 카탈로그 없이 낱개 PNG와 Info.plist의 `CFBundleIcons`·`CFBundleIcons~ipad`로 들어간다.

번들 ID는 `com.hanggok.atlan`, 홈 화면 이름은 "Atlan"이다(`platform/ios/CMakeLists.txt`). 2026-11-26 전 빌드는 `org.example.atlan-runtime`이었다. 번들 ID가 바뀌면 다른 앱으로 설치되므로 Documents가 새로 비어 있다. ATLAN 폴더와 `native.sig`, `input.cfg`를 새 앱의 Documents(파일 앱의 나의 iPhone › Atlan)에 다시 넣는다.

## 화면

런처는 내비게이션을 시작하기 전까지 세로로도 돌아가고, 내비게이션이 시작되면 가로(`LandscapeLeft/Right`)로 고정된다. iPhone·iPad 모두 지원한다(`TARGETED_DEVICE_FAMILY` 1,2, 방향 고정을 위해 `UIRequiresFullScreen`). 렌더링 해상도의 기본값은 기기 실제 픽셀(points × `nativeScale`)이고, 발열 대비 내부 해상도 배율은 `engine/shell/render_config.hpp`의 `render_size`에서 0.25~1.0으로 줄일 수 있게 자리만 두었다(UI 옵션은 아직 없다). 그래픽 서비스(M4) 전까지 이 값은 쓰이지 않는다.

## 실행 중 동작

- 게스트 화면은 설정의 화면 크기와 상관없이 기기 화면 전체에 늘려 표시한다.
- 노치가 있는 iPhone에서 런처 설정의 "노치 여백"이 켜져 있으면(기본값) 게스트 화면은 노치 쪽 띠를 뺀 safe area 폭으로 잡혀 버튼과 글자가 노치에 가리지 않고, 띠는 검게 남는다. 개발자 설정의 실험 기능 "노치 밑 지도"(`launcher.notchMap`, 기본값 꺼짐)를 켜면 노치 띠에 `MarginGlBackend`(`engine/include/atlan/gl_margin.hpp`)가 게스트가 화면 가장자리 밖으로 그린 지도를 보여 주고, 화면 폭 전체를 덮는 배경(하늘, 메뉴 배경)은 띠까지 늘린다. 렌더 대상(drawable)은 게스트 폭 + 띠 폭이고 레이어는 뷰 전체를 덮으며, 터치는 그대로 게스트 영역(`shownRect`)으로 매핑한다. 기기를 반대 방향 가로로 돌리면 띠가 반대쪽으로 옮겨진다.
- 게스트가 실행 중이면 화면 자동 잠금을 끈다(`idleTimerDisabled`).
- 게스트의 종료 대화상자에서 종료를 고르면 앱도 끝난다. 안드로이드 쪽 Java 코드처럼 `MainActivity.nativeIsProgramExit`를 1초마다 확인하고(`SessionConfig::on_guest_exit`), 참이면 세션을 끝낸 뒤 프로세스를 종료한다.
- 디버깅용으로 한 번 실행한 동안의 모든 로그 줄을 걸러내지 않고 `Documents/logs/session-<시각>.log`에 줄마다 바로 기록한다(앞의 숫자는 시작 후 경과 초, 최근 5개만 보관). 충돌하거나 종료해도 그때까지의 내용이 남는다. `platform/ios/main.mm`의 `kWriteSessionLog`를 `false`로 바꾸면 꺼진다.

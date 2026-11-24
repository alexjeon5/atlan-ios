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

- 네이티브 라이브러리: 파일 앱 선택기로 가져와 `Application Support/guest.so`로 교체 저장한다(수십 MB 규모라 복사). 앱과 IPA에 번들하지 않는다.
- 지도: 복사하지 않는다. 사용자가 파일 앱·Finder로 앱 Documents 폴더에 직접 넣거나 옮기고(`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`), Documents에서 가장 큰 일반 파일을 그 자리에서 사용한다. 교체는 같은 폴더의 파일을 바꿔 넣는 것이다. 앱은 크기와 앞·뒤 4KB 읽기 가능 여부만 검증하고(`shell/data_file`), 형식별 헤더 검증은 형식이 정해지면 `magic`으로 추가한다. 잘못된 파일은 사유와 함께 거부하며 보관·롤백하지 않는다.
- 실행: 해석기가 `diagnostic_traps`로 로드하고 libc·문자열·printf·힙·mmap·스레드·mutex·조건변수·세마포어·가상 파일·스레드 키·android_log·OpenSL ES(Apple AudioQueue 싱크) 서비스를 등록한 뒤 생성자를 차례로 실행하고, `JNI_OnLoad`가 있으면 최소 JavaVM/JNIEnv로 호출하고, `MainActivity.nativeInit`을 실행하며(Documents를 파일 루트로 사용, `setSurfaces`는 호출만 기록), 이어서 게스트 스레드와 오디오 콜백을 한 번 `pump`한다. 테스트 절차는 `docs/testing.md`. 서비스 구성은 `tools/elf.cpp`와 같아야 한다. 중지는 생성자 경계에서 협력적으로 처리된다(호출당 예산 3,000,000 명령).
- 지도를 게스트 파일 접근에 연결하는 가상 파일 디스크립터는 M4b 범위이며 아직 없다.
- 위치 같은 호스트 서비스(CoreLocation)는 `shell` 계층에 서비스 자리만 두고 구현하지 않았다.

CI는 `Release-iphoneos` 앱을 `Payload/`로 압축한 서명 없는 IPA(`atlan-runtime-ios-unsigned`)를 아티팩트로 올린다. 사이드로딩 도구(iloader 등)로 서명·설치한다.

## 화면

가로 모드 전용(`LandscapeLeft/Right`)이며 iPhone·iPad 모두 지원한다(`TARGETED_DEVICE_FAMILY` 1,2, iPad는 가로 전용을 위해 `UIRequiresFullScreen`). 렌더링 해상도의 기본값은 기기 실제 픽셀(points × `nativeScale`)이고, 발열 대비 내부 해상도 배율은 `shell/render_config.hpp`의 `render_size`에서 0.25~1.0으로 줄일 수 있게 자리만 두었다(UI 옵션은 아직 없다). 그래픽 서비스(M4) 전까지 이 값은 쓰이지 않는다.

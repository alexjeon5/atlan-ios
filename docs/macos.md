# macOS 로컬 테스트

ARM64 iOS가 제품 타깃이고, macOS 앱과 CLI는 같은 CPU 코어를 테스트하는 호스트다. 인텔 맥에서는 x86_64 호스트 코드로 ARM32 명령을 해석한다. ARM32 → ARM64로 미리 번역한 코드를 x86_64에서 직접 실행하는 구조는 아니다.

Xcode Command Line Tools와 CMake 3.22 이상으로 빌드한다. 아래 명령은 현재 맥의 기본 아키텍처를 선택하므로 인텔 맥에서도 사용할 수 있다.

```sh
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Debug -DATLAN_BUILD_MACOS=ON
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
./build-macos/atlan-poc
open build-macos/platform/macos/AtlanRuntimeMac.app
```

인텔 전용 빌드는 별도 디렉터리에 `-DCMAKE_OSX_ARCHITECTURES=x86_64`, 범용 빌드는 `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`를 지정한다. 앱과 CLI 모두 동일한 아키텍처 설정을 따른다. 기본 macOS 최소 실행 버전은 12.0이며 `CMAKE_OSX_DEPLOYMENT_TARGET`으로 변경할 수 있다. 사용한 SDK와 컴파일러가 해당 OS를 지원해야 한다.

CI는 push마다 macOS 26 Apple Silicon 러너에서 CPU·ELF 정확성 검사를 실행한다(x86_64는 Linux 러너가 맡는다). 범용 빌드와 두 아키텍처 포함 여부 검사는 Actions에서 워크플로를 수동 실행(Run workflow)할 때만 돈다. 그때 나오는 `atlan-runtime-macos-universal` 아티팩트에는 로컬 테스트용 앱, `bin/atlan-poc`, `bin/atlan-elf`가 있다. 앱은 임시 서명만 적용하며 Developer ID 공증된 배포본은 아니다.

최신 OS에서 빌드·테스트하는 것과 최소 지원 OS에서 실제 실행하는 것은 별개다. macOS 12–15 실기기에서의 실행은 아직 검증하지 않았다. GUI와 CLI는 합성 프로그램만 실행하며 원본 앱 실행과 IPA 패키징은 후속 작업이다.

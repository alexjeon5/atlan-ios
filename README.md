# atlan-ios

ARM32 Android 바이너리를 네이티브 ARM64 iOS 앱 안에서 실행하기 위한 독립 호환 런타임이다. Android OS와 Linux 커널을 부팅하지 않는다.

현재는 Kernarium에서 분리한 AArch32 디코더와 정수·분기·메모리 의미론을 이용하는 인터프리터 PoC다. 합성 ARM32 프로그램을 실행해 `42`를 계산하고, 메모리에 저장한 뒤 SVC 이벤트로 호스트에 제어를 반환한다. ARM, Thumb, Thumb-2와 IT 조건부 실행을 지원한다. 원본 앱을 실행하는 단계에는 아직 도달하지 않았다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/atlan-poc
```

C++20과 CMake 3.22 이상이 필요하다. Apple 전용 코드는 `platform/ios`와 `platform/macos`에 분리되어 있다. iOS 데모는 같은 합성 프로그램의 결과를 표시하며, Xcode 생성·컴파일 방법은 [iOS 빌드](docs/ios.md)에 있다. 인텔 맥·Apple Silicon의 CLI와 네이티브 테스트 앱은 [macOS 로컬 테스트](docs/macos.md)를 참고한다. 64비트 전용 Android 기기용 arm64-v8a APK는 [Android](docs/android.md)에 있다(`platform/android`, `platform/gles`).

ELF32 공유 라이브러리 로더와 scalar ARM/Thumb 함수 호출을 제공한다. [ELF 지원 범위](docs/elf.md)와 `atlan-elf --help`를 참고한다. Bionic·JNI·DEX, 그래픽·오디오 브리지, AOT와 최종 IPA는 후속 작업이다. VFP/NEON은 아직 실행하지 않으며 지원하지 않는 명령은 명시적 이벤트로 중단한다. JIT 권한이나 런타임 실행 메모리 할당은 사용하지 않는다.

이 저장소는 Kernarium의 GitHub 포크가 아니며 별도 Git 이력을 가진다. 소스는 GPL-2.0-or-later이고, 재사용한 파일의 SPDX를 유지한다. [구성요소 분석과 출처](docs/kernarium.md), [모듈 경계](docs/architecture.md)를 참고한다. 원본 애플리케이션과 분석 산출물은 로컬 입력이며 공개 저장소에 포함하지 않는다.

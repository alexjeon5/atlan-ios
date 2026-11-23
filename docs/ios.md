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

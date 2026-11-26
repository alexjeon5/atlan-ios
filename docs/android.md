# Android (arm64-v8a)

64비트 전용 Android 기기에서 원본 ARM32 네이티브 라이브러리를 돌리는 APK다. iOS와 같은 엔진(인터프리터, 네이티브 대체, 런타임 JIT 없음)과 앱 어댑터를 그대로 쓰고, 기기 쪽 서비스만 Android API로 바꾼다. APK에는 `lib/arm64-v8a`만 들어가며 원본 앱에 없던 화면(런처, 설정, 안내 페이지, 디버그 표시)은 없다.

APK는 로컬 설치용이다. 공개 저장소·릴리스·아티팩트에 올리지 않는다. 원본 패키지와 데이터는 사용자가 기기에 직접 넣는다.

## 구조

| 위치 | 분류 | 역할 |
| --- | --- | --- |
| `platform/gles/` | 엔진 쪽 호스트 계층 | `GlesBackend`(게스트 GLES2 호출을 기기의 GLES2로 그대로 전달), `EglHost`(EGL 컨텍스트와 창 표시). Linux에서는 Mesa로 같은 코드를 테스트한다 |
| `platform/android/` | 엔진 쪽 호스트 계층 | `GuestActivity`·`HostHttp`(Java), `guest_host.cpp`(JNI, 세션 하나), `OpenSlesAudioSink`, 빌드 스크립트 |
| `adapters/atlan/android/` | 어댑터 | `AtlanActivity`(데이터 폴더, 뒤로 두 번 종료), 매니페스트·문구, `make_app_adapter` |

플랫폼 계층은 앱을 모른다. 앱은 `platform/android/android_app.hpp`의 `make_app_adapter` 하나로 `AppAdapter`를 넘기고, Java 쪽은 `GuestActivity`를 상속해 데이터 위치만 정한다.

## 동작

- **그래픽**: 셰이더는 게스트의 GLSL ES 원문 그대로, 압축 텍스처는 드라이버로 그대로 간다(번역 없음). 게스트의 기본 프레임버퍼는 게스트가 고른 표면 크기의 오프스크린 프레임버퍼(색 텍스처와 깊이, `EXT_multisampled_render_to_texture`가 있으면 원본 EGL 설정처럼 4x)다. `eglSwapBuffers`마다 같은 공유 그룹의 두 번째 컨텍스트가 그 텍스처를 창 전체에 늘려 그린다. 창 표면은 그리는 동안에만 잠금 아래에서 바인딩되므로, 앱이 뒤로 가거나 화면이 꺼져 표면이 사라져도 게스트의 GL 상태와 프레임은 영향을 받지 않는다.
- **오디오**: 엔진의 OpenSL ES 계층이 기기의 OpenSL ES 플레이어(버퍼 큐, 20ms 단위)로 낸다. 장치가 끝낸 버퍼의 PCM만 재생된 양으로 센다.
- **터치**: `SurfaceView`의 터치를 손가락 번호(Android pointer id) 그대로 입력 큐에 넣고, 화면 전체를 게스트 표면에 대응시킨다.
- **위치**: 위치 권한을 받은 뒤 `LocationManager`의 GPS 제공자를 쓴다(Google Play 서비스 의존 없음). 바인딩은 iOS와 같은 `input.cfg` 형식이다.
- **네트워크**: `http://`는 엔진의 소켓 클라이언트, `https://`는 `HttpURLConnection`. 연결 상태는 `ConnectivityManager`의 기본 네트워크 콜백으로 알린다. 게스트 소켓은 호스트 소켓 그대로다.
- **화면**: 가로 고정, 전체 화면, 화면 꺼짐 방지. `targetSdkVersion`이 34라서 Android 15에서도 노치 쪽은 시스템이 비워 둔다.
- **종료**: 게스트의 종료 대화상자, 또는 2초 안에 뒤로 두 번(원본과 같은 안내 문구). 프로세스를 끝내므로 다음 실행은 새로 시작한다. 지도 화면의 홈(FINE DRIVE) 버튼은 원본 폰 앱처럼 아무 동작도 하지 않는다(로그만).
- **로그**: logcat 태그 `atlan`, 그리고 `Android/data/atlan.runtime/files/logs/session-*.log`(최근 5개).

## 데이터

원본처럼 내장 공유 저장소 최상위의 `ATLAN` 폴더(`/sdcard/ATLAN`)를 쓴다. 처음 실행하면 위치 권한과 "모든 파일 접근"(Android 11+) 설정 화면이 뜬다. 권한 없이 쓰려면 앱 전용 폴더 `/sdcard/Android/data/atlan.runtime/files/ATLAN`에 넣어도 되고, 그쪽이 있으면 먼저 쓴다. 둘 다 없으면 두 경로를 알려 주고 끝난다.

입력 바인딩과 네이티브 대체 시그니처는 데이터 루트(`ATLAN`의 부모 폴더)의 `input.cfg`·`native.sig`, 없으면 APK의 `assets/`에서 읽는다. 둘 다 앱 고유 값이라 저장소에는 없고, 빌드할 때 `--asset`으로 넣는다.

## 빌드

Gradle 없이 SDK·NDK만 쓴다. 필요한 것: Android SDK(`platforms/android-*`, `build-tools/*`), NDK, CMake, JDK 11 이상, Python 3.

```sh
export ANDROID_HOME=~/Library/Android/sdk          # 예
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/<버전>    # 생략하면 가장 새 NDK
adapters/atlan/android/build.sh --out build-android \
  --asset ../private/input.cfg --asset ../private/native.sig
# → build-android/atlan.runtime-arm64-v8a.apk (디버그 키 서명)
```

스크립트(`platform/android/build_apk.sh`)는 네이티브 라이브러리를 CMake로 만들고(NDK 툴체인, 최소 API 26, 16KB 페이지 정렬, `JNI_OnLoad`만 export), `aapt`로 매니페스트·리소스를, `javac`+`d8`로 Java를 묶은 뒤 `zipalign`과 `~/.android/debug.keystore`로 서명한다(키가 없으면 만든다).

NDK가 컴파일러를 제공하지 않는 호스트(arm64 Linux 서버)에서는 호스트 clang에 NDK의 sysroot·compiler-rt·libunwind를 쓰는 `platform/android/host-clang.toolchain.cmake`로 자동 전환한다. 그런 호스트에서는 SDK의 `aapt`·`zipalign` 대신 배포판 패키지(`aapt`, `zipalign`)를 PATH에서 찾고, 배포판 `aapt`는 API 34 이후 플랫폼의 리소스 표를 읽지 못하므로 `platforms/android-33`을 둔다(`ANDROID_PLATFORM_JAR`로 지정 가능).

CI(`android-build` 잡)는 앱 고유 파일 없이 APK를 컴파일만 하고 올리지 않는다.

## 설치

```sh
adb install -r build-android/atlan.runtime-arm64-v8a.apk
adb push ATLAN /sdcard/ATLAN                                   # 처음 한 번, 수 GB
adb shell appops set atlan.runtime MANAGE_EXTERNAL_STORAGE allow   # 설정 화면 대신
adb logcat -s atlan                                            # 로그
```

## 검증 상태

- Linux(Mesa llvmpipe, 서버): `gles-backend-tests`가 합성 호출을 픽셀로 확인한다. `ATLAN_GLES=DIR atlan-session "" DATA_ROOT`로 실제 게스트를 같은 `GlesBackend`로 돌려, 약관 화면·지도 화면(실시간 교통, 3D 랜드마크)·메인 메뉴가 셰이더 오류와 건너뛴 draw 없이 그려지는 것을 프레임으로 확인했다.
- APK: 서버에서 빌드, `apksigner verify`(v2·v3) 통과, `lib/arm64-v8a`만 포함, 네이티브 라이브러리의 의존은 시스템 라이브러리(`libEGL`, `libGLESv2`, `libOpenSLES`, `liblog`, `libandroid`, `libc`, `libm`, `libdl`)뿐, Java 네이티브 메서드 시그니처와 `RegisterNatives` 표 일치.
- 실기기 실행, OpenSL ES 출력, GPS, HTTPS, 창 표시와 수명주기는 아직 기기에서 확인하지 않았다(이 서버에는 KVM과 binder가 없어 에뮬레이터를 돌릴 수 없다).

## 아직 없는 것

- 백그라운드 위치(포그라운드 서비스). 앱이 뒤로 가면 Android가 위치 갱신을 줄인다.
- 뒤로 간 동안에도 게스트는 계속 프레임을 그린다(표시만 하지 않는다). 배터리를 위해 멈출지는 기기에서 확인한 뒤 정한다.
- 원본의 NMEA 수신(`OnNmeaMessageListener`) 대신 위치 값으로 만든 문장을 쓴다(iOS와 같음).

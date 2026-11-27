# 입력 연결 (터치, 위치)

호스트 입력을 게스트의 네이티브 메서드 호출로 바꾸는 계층이다. 메커니즘(`engine/shell/input.*`)은 엔진 쪽이고 앱을 모른다. 어떤 메서드가 어떤 인자를 받는지는 어댑터 값이며, 바인딩 텍스트로 주입한다. 실제 앱용 바인딩은 저장소에 넣지 않는다.

## 흐름

1. UI 스레드가 `InputQueue`에 `TouchEvent`·`LocationFix`를 넣는다. 같은 손가락의 연속 move와 연속 위치 값은 최신 값으로 합쳐지고, 큐가 가득 차면(256) move와 위치만 버린다. down/up/cancel은 항상 들어간다.
2. 게스트 스레드(`GuestSession`)가 프레임마다 `renderGLSF` 전에 큐를 비우고, 이벤트마다 바인딩된 네이티브 메서드를 한 번 호출한다.
3. 대상 주소는 `RegisterNatives`로 등록된 (클래스, 이름, 시그니처)에서 찾고, 없으면 `Java_<클래스>_<이름>` export에서 찾는다. `this`는 그 클래스의 호스트 객체 하나를 재사용한다. export 이름에는 시그니처가 없어서 같은 앱의 다른 빌드가 같은 이름으로 다른 매개변수를 받을 수 있다. 그래서 JNI_OnLoad 뒤에 규칙을 한 번 고른다(아래 "빌드별 규칙").
4. 바인딩이 없는 이벤트, 등록도 export도 안 된 메서드는 로그에 한 번 남기고 버린다(`input: …`). 호출이 게스트 안에서 멈추면 다른 네이티브 호출처럼 세션이 실패로 끝난다.

`SessionConfig::interactive`가 켜져 있으면 첫 `render_frames` 프레임과 기존 `pump`·오디오 단계가 끝난 뒤(로그 `interactive`), 중지될 때까지 입력 전달 → `renderGLSF` → swap → 게스트 스레드 `pump` → 오디오 콜백을 반복한다. 프레임 간격의 하한은 `frame_interval_ms`(기본 33ms)다. `render_on_demand`가 켜져 있으면(iOS 앱) `renderGLSF`는 게스트가 프레임을 요청했을 때만 부른다. 요청은 게스트 GL 스레드의 `callBackGLThread` 콜백이고, 안드로이드 쪽 Java 코드처럼 `isStateReady` → `isGLInit` → `setState(3)` 순서로 응답한다(`RENDERMODE_WHEN_DIRTY`). 요청이 없으면 `idle_redraw_ms`(기본 1초)마다 같은 질문을 직접 해서 준비된 경우에만 그린다. 준비되지 않은 상태에서 `renderGLSF`를 부르면 화면을 검게 지우기만 하므로 그 경우에는 그리지 않는다. 지도 화면에서는 게스트가 프레임이 끝날 때마다 바로 다음 프레임을 요청하므로 그리는 횟수는 줄지 않는다(서버 45초 측정: 요청 376회, 그린 프레임 373개). `ATLAN_INTERACTIVE_SECONDS=N`과 `ATLAN_ON_DEMAND=1`로 헤드리스에서 확인한다.

화면이 거의 바뀌지 않으면 프레임 간격을 늘린다(`still_frame_interval_ms`, iOS 100ms). `FrameHashBackend`(`engine/shell/frame_hash.hpp`)가 프레임의 GL 호출을 하나씩 해시해 직전 프레임과 비교하고, 바뀐 호출이 16개 이하면 같은 화면으로 본다. 텍스처·버퍼는 이름 대신 마지막으로 올린 내용(과 파라미터)으로, 클라이언트 배열 포인터는 빼고 비교하므로 게스트가 매 프레임 같은 내용의 텍스처를 새로 만들어도 같은 화면이다. 최근 `still_after_frames`(10)프레임 중 바뀐 프레임이 5분의 1 이하면 그 간격, 최근 40프레임 중 4분의 1 이하면 두 배를 쓴다. GPS 수신이나 라벨 하나처럼 가끔 바뀌는 프레임은 느린 간격을 유지하고, 지도가 움직이면 몇 프레임 안에 원래 간격으로 돌아오며, 터치는 바로 돌아온다. 회전하는 3D 랜드마크처럼 작은 애니메이션은 느린 간격으로 움직인다. 서버(정차, GPS 수신 중): 약 14fps → 약 6fps, 렌더 시간 비율 약 80% → 34%. 주행 중에는 간격이 그대로였다. 호스트는 `GuestSession::set_frame_interval_floor_ms()`로 하한을 더 올릴 수 있고, iOS 앱은 `thermalState`(fair 67ms, serious 100ms, critical 200ms)와 저전력 모드(100ms)를 여기에 넘긴다. `perf frames:` 줄 끝의 `still frames`와 `interval`이 그 구간의 같은 화면 프레임 수와 현재 간격이고, `host floor`가 호스트가 건 하한(iOS: 발열·저전력 모드)이다. 헤드리스에서는 `ATLAN_STILL_MS=N`.

## 바인딩 텍스트

한 줄에 규칙 하나, `#` 뒤는 주석이다.

```text
touch[.down|.move|.up|.cancel] CLASS METHOD SIGNATURE ARG...
location CLASS METHOD SIGNATURE ARG...
actions DOWN MOVE UP CANCEL
```

- `ARG`는 시그니처의 매개변수마다 하나다. 값 이름이거나(뒤에 `*배율` 가능), 숫자 상수다.
- 터치 값: `action`(단계별 코드, 기본 0/2/1/3 = down/move/up/cancel), `pointer`(손가락 번호), `x`·`y`(게스트 표면 픽셀, 좌상단 원점), `time`(단조 시간 ms).
- 위치 값: `latitude`·`longitude`(도), `altitude`(m), `speed`(m/s, 모르면 음수), `course`(도, 모르면 음수), `accuracy`(m), `time`(Unix epoch ms).
- 위치 문장 값: `gga`, `gsa`, `gsv`, `rmc`. `Ljava/lang/String;` 매개변수에만 쓸 수 있고, 위치 값으로 만든 NMEA 0183 문장(체크섬, CR LF 포함) 하나를 String 객체로 넘긴다. `gsv`는 문장이 여러 개(지금은 2개)라서 그 규칙은 위치 값마다 문장 수만큼 호출된다. String 객체는 인자마다 하나를 만들어 내용만 바꿔 재사용한다.
- `touch`는 네 단계 모두에, `touch.up` 같은 형태는 한 단계에만 적용되고 그 단계의 `touch` 규칙을 대신한다. 같은 형태의 터치 규칙이 여러 개면 빌드별 대안이다(아래). `location` 규칙은 여러 개를 쓸 수 있고 위치 값마다 순서대로 모두 호출된다.
- 숫자 값은 시그니처의 타입으로 변환한다. `Z B C S I`는 반올림한 32비트 정수(범위 밖은 포화), `J`는 64비트 정수, `F`·`D`는 부동소수다. 그 밖의 매개변수 타입은 거부한다.
- 인자 수가 시그니처와 다르거나 알 수 없는 이름이 있으면 `line N: 이유`로 파일 전체를 거부한다.

합성 예:

```text
touch example/Input onTouch (IIJFF)I action pointer time x y
location example/Input onFix (DDFJ)V latitude longitude speed*3.6 time
location example/Input onSentence (Ljava/lang/String;)V gga
location example/Input onSentence (Ljava/lang/String;)V gsa
location example/Input onSentence (Ljava/lang/String;)V gsv
location example/Input onSentence (Ljava/lang/String;)V rmc
```

### 빌드별 규칙

어댑터는 앱 패키지의 `classes*.dex`에서 메서드 표(클래스, 이름, 시그니처)를 읽어 준다(`AppAdapter::java_methods`, `shell/dex.hpp`). 세션은 JNI_OnLoad 뒤에 이 표와 `RegisterNatives`로 등록된 메서드를 합쳐 규칙을 고른다(`choose_bindings`).

- 앱이 그 (클래스, 이름)을 다른 시그니처로만 선언했으면 그 규칙은 빼고 로그에 `input: skip CLASS.NAMESIG, the app declares …`를 남긴다.
- 터치 단계마다 남은 대안 중 파일에서 먼저 나온 것 하나만 쓴다. 남은 것이 없으면 그 단계는 바인딩이 없다.
- 남은 `location` 규칙은 모두 호출된다.
- 선언 정보가 없으면(패키지가 아닌 라이브러리를 직접 실행, dex가 손상됨) 모든 규칙이 남고, 터치는 첫 대안을 쓴다.

그래서 한 파일에 두 빌드의 규칙을 함께 적어 둘 수 있다. 합성 예:

```text
touch example/Input onTouch (IIJFF)I 1 action time x y   # 이전 빌드
touch example/Input onTouch (IJFF)V action time x y      # 매개변수가 바뀐 빌드
```

AAPCS32에서는 두 시그니처의 인자 워드 배치가 같아도(`I`가 하나 줄면 `J`가 짝수 워드로 밀려 같은 자리에 온다) 앱이 각 워드를 다르게 읽으므로, 맞지 않는 규칙으로 호출하면 오류 없이 엉뚱한 값이 들어간다.

호출 인자는 AAPCS32(softfp) 규칙으로 놓는다. `env`, `this` 다음에 매개변수가 오고, 64비트 값(`J`, `D`)은 짝수 번째 워드에서 시작하는 두 워드(하위 먼저)다.

## NMEA 문장

CoreLocation은 위성 정보를 공개하지 않는다. 그래서 위치 값의 수평 정확도로 하늘을 정하고(`sky_for`), 네 문장이 모두 같은 하늘을 말한다. 오차가 작을수록 쓰는 위성이 많고 신호가 세다. 4m 이하는 12개, 18m 이하 8개, 50m 이하 5개, 100m 이하 3개이고, 보이는 위성은 쓰는 위성보다 2개 많다(쓰지 않는 위성은 신호가 약하다). 100m를 넘으면 위성 fix가 없는 것으로 보고 약한 위성 4개만 보이게 한다. HDOP = 0.5 + 정확도 × 0.045(0.6~9.9)다. 10m에서 0.95, 33m에서 2, 100m에서 5다. CoreLocation의 정확도는 보수적이라(뚫린 도로에서 5~16m) 같은 상황의 안드로이드 GPS는 HDOP 0.7~1.3을 보이고, fix를 잡고 있는 동안 2를 거의 넘지 않는다. 앱이 HDOP을 믿고 판단을 바꾸므로(HDOP이 2를 넘으면 방향 변화를 조금씩만 받아들이는 앱이 있다) 실제 수신기와 비슷한 값이어야 한다. 처음에는 정확도 ÷ 5m로 했다가 커브에서 차 방향이 늦게 따라왔다.

- `gga`: fix quality(1, fix가 없으면 0), 쓰는 위성 수, HDOP, 고도.
- `gsa`: 3D fix(없으면 1)와 쓰는 위성 번호, PDOP·HDOP·VDOP, NMEA 4.1 시스템 ID(1).
- `gsv`: 보이는 위성의 위치와 신호 세기, 4개씩 한 문장.
- `rmc`: 상태 A(fix가 없으면 V), 속도는 노트 단위이고 모르면 0, 방위는 모르면 빈 칸이다. fix가 없으면 속도·방위를 비우고 마지막 위치를 그대로 둔다.
- 시각은 fix 시각의 UTC다.

위치 값이 3초 동안 오지 않으면(터널, 지하 주차장) 세션이 1초마다 마지막 위치로 "신호 없음" 위치 값(정확도 -1)을 만들어 같은 규칙으로 보낸다(`LocationWatch`). 이때는 보이는 위성도 없고 RMC 상태는 V다. 안드로이드 GPS도 하늘을 잃은 뒤 이렇게 계속 보고한다. 위치 값이 다시 오면 바로 정상 문장으로 돌아간다. 그래서 위치를 고정해 넣는 기능은 같은 값을 1초마다 다시 넣어야 한다.

안드로이드 수신기는 한 주기에 GGA, GSA, GSV…, RMC 순으로 문장을 보내고, 받는 쪽은 RMC에서 그 주기를 마무리하는 경우가 많다. 그래서 규칙도 이 순서로 적는다. 위성 수를 GGA가 아니라 GSV에서 읽는 앱이 있으므로 GGA·RMC만 보내면 위치와 속도는 보여도 위성 0개인 신호로 취급될 수 있다.

## iOS 앱

- 바인딩 파일은 앱 Documents 폴더의 `input.cfg`다. 실행 시 읽고 로그에 `input bindings ok`, `input bindings missing`, `input bindings rejected: …` 중 하나를 남긴다.
- 위치: 실행할 때 사용 중 권한을 요청하고(`NSLocationWhenInUseUsageDescription`) `kCLLocationAccuracyBestForNavigation`으로 갱신을 받는다. 수평 정확도가 음수인 값은 버린다. 거부되면 로그에 `location denied`가 남는다. 백그라운드 위치는 아직 없다.
- 터치: 로그 영역이 화면을 덮고 있어서 상단의 로그 버튼으로 숨긴 뒤에 표면이 터치를 받는다. 좌표는 게스트 표면(`setSurfaces`로 보고된 크기)이 실제로 표시되는 사각형 기준 픽셀로 바꾼다. iOS 앱에서는 항상 뷰 전체다. 사각형 밖의 점은 가장자리로 붙는다. 멀티터치는 손가락마다 0부터 빈 번호를 준다.

## 헤드리스 확인

`ATLAN_INPUT_CFG=파일 atlan-session LIBRARY DATA_ROOT`는 바인딩을 읽고 첫 프레임 전에 위치 값 하나와 탭(down, up) 하나를 넣는다. 탭 위치는 `ATLAN_TAP=X,Y`(게스트 화면 픽셀, 기본 400,240)이고 `ATLAN_TAP=none`이면 탭을 넣지 않는다. 로그의 `input: first … event -> …` 줄과 그 뒤 게스트 동작으로 바인딩을 확인한다.

`ATLAN_INTERACTIVE_SECONDS`로 오래 돌릴 때는 `ATLAN_TAP_FILE=파일`을 주면 실행 중에 그 파일 끝에 붙인 `X,Y[,누름ms]` 줄마다 탭을 하나씩 넣는다(누름 기본 `ATLAN_TAP_HOLD_MS`, 80). 화면을 보면서 다음 탭을 고를 때 쓴다.

주행은 `ATLAN_INTERACTIVE_SECONDS`와 함께 `ATLAN_DRIVE=위도,경도,방위,시속[,시작ms[,끝ms]]`로 흉내 낸다. 시작ms(기본 0)부터 끝ms(기본 끝까지)까지 1초마다 그 방위로 직진한 위치 값(정확도 8m)을 벽시계 시각으로 넣는다. 끝ms에서 멈추면 신호가 끊긴 것처럼 보인다.

## 검증 상태

`tests/input_tests.cpp`가 큐 합치기, 바인딩 파싱·거부, 빌드별 대안 고르기, NMEA 문장(GGA, GSA, GSV, RMC), 문장 여러 개인 규칙의 호출 횟수, 인자 워드 배치, 합성 게스트 함수로의 전달을 확인한다. `tests/package_tests.cpp`가 합성 dex의 메서드 표 읽기를 확인한다. 실제 라이브러리에서 인자 순서와 단위가 맞는지, 실기기에서 CoreLocation과 터치가 동작하는지는 이 테스트로 확인되지 않는다.

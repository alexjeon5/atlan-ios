# 입력 연결 (터치, 위치)

호스트 입력을 게스트의 네이티브 메서드 호출로 바꾸는 계층이다. 메커니즘(`shell/input.*`)은 엔진 쪽이고 앱을 모른다. 어떤 메서드가 어떤 인자를 받는지는 어댑터 값이며, 바인딩 텍스트로 주입한다. 실제 앱용 바인딩은 저장소에 넣지 않는다.

## 흐름

1. UI 스레드가 `InputQueue`에 `TouchEvent`·`LocationFix`를 넣는다. 같은 손가락의 연속 move와 연속 위치 값은 최신 값으로 합쳐지고, 큐가 가득 차면(256) move와 위치만 버린다. down/up/cancel은 항상 들어간다.
2. 게스트 스레드(`GuestSession`)가 프레임마다 `renderGLSF` 전에 큐를 비우고, 이벤트마다 바인딩된 네이티브 메서드를 한 번 호출한다.
3. 대상 주소는 `RegisterNatives`로 등록된 (클래스, 이름, 시그니처)에서 찾고, 없으면 `Java_<클래스>_<이름>` export에서 찾는다. `this`는 그 클래스의 호스트 객체 하나를 재사용한다.
4. 바인딩이 없는 이벤트, 등록도 export도 안 된 메서드는 로그에 한 번 남기고 버린다(`input: …`). 호출이 게스트 안에서 멈추면 다른 네이티브 호출처럼 세션이 실패로 끝난다.

`SessionConfig::interactive`가 켜져 있으면 첫 `render_frames` 프레임과 기존 `pump`·오디오 단계가 끝난 뒤(로그 `interactive`), 중지될 때까지 입력 전달 → `renderGLSF` → swap → 게스트 스레드 `pump` → 오디오 콜백을 반복한다. 프레임 간격의 하한은 `frame_interval_ms`(기본 33ms)다. `render_on_demand`가 켜져 있으면(iOS 앱) `renderGLSF`는 게스트가 프레임을 요청했을 때만 부른다. 요청은 게스트 GL 스레드의 `callBackGLThread` 콜백이고, 안드로이드 쪽 Java 코드처럼 `isStateReady` → `isGLInit` → `setState(3)` 순서로 응답한다(`RENDERMODE_WHEN_DIRTY`). 요청이 없어도 `idle_redraw_ms`(기본 1초)마다 한 번은 그린다. 지도 화면에서는 게스트가 프레임이 끝날 때마다 바로 다음 프레임을 요청하므로 그리는 횟수는 줄지 않는다(서버 45초 측정: 요청 376회, 그린 프레임 373개). `ATLAN_INTERACTIVE_SECONDS=N`과 `ATLAN_ON_DEMAND=1`로 헤드리스에서 확인한다.

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
- 위치 문장 값: `gga`, `rmc`. `Ljava/lang/String;` 매개변수에만 쓸 수 있고, 위치 값으로 만든 NMEA 0183 문장(체크섬, CR LF 포함) 하나를 String 객체로 넘긴다. String 객체는 인자마다 하나를 만들어 내용만 바꿔 재사용한다.
- `touch`는 네 단계 모두에, `touch.up` 같은 형태는 한 단계에만 적용된다. `location` 규칙은 여러 개를 쓸 수 있고 위치 값마다 순서대로 모두 호출된다.
- 숫자 값은 시그니처의 타입으로 변환한다. `Z B C S I`는 반올림한 32비트 정수(범위 밖은 포화), `J`는 64비트 정수, `F`·`D`는 부동소수다. 그 밖의 매개변수 타입은 거부한다.
- 인자 수가 시그니처와 다르거나 알 수 없는 이름이 있으면 `line N: 이유`로 파일 전체를 거부한다.

합성 예:

```text
touch example/Input onTouch (IIJFF)I action pointer time x y
location example/Input onFix (DDFJ)V latitude longitude speed*3.6 time
location example/Input onSentence (Ljava/lang/String;)V gga
location example/Input onSentence (Ljava/lang/String;)V rmc
```

호출 인자는 AAPCS32(softfp) 규칙으로 놓는다. `env`, `this` 다음에 매개변수가 오고, 64비트 값(`J`, `D`)은 짝수 번째 워드에서 시작하는 두 워드(하위 먼저)다.

## NMEA 문장

CoreLocation은 위성 정보를 주지 않는다. 그래서 `gga`는 fix quality 1, 위성 수 8(명목값), HDOP = 정확도 ÷ 5m(0.5~50)로 채운다. `rmc`의 속도는 노트 단위이고 모르면 0, 방위는 모르면 빈 칸이다. 시각은 fix 시각의 UTC다. 앱이 다른 문장(GSA, GSV)이나 다른 값을 요구하는지는 실제 라이브러리로 확인해야 한다.

## iOS 앱

- 바인딩 파일은 앱 Documents 폴더의 `input.cfg`다. 실행 시 읽고 로그에 `input bindings ok`, `input bindings missing`, `input bindings rejected: …` 중 하나를 남긴다.
- 위치: 실행할 때 사용 중 권한을 요청하고(`NSLocationWhenInUseUsageDescription`) `kCLLocationAccuracyBestForNavigation`으로 갱신을 받는다. 수평 정확도가 음수인 값은 버린다. 거부되면 로그에 `location denied`가 남는다. 백그라운드 위치는 아직 없다.
- 터치: 로그 영역이 화면을 덮고 있어서 상단의 로그 버튼으로 숨긴 뒤에 표면이 터치를 받는다. 좌표는 게스트 표면(`setSurfaces`로 보고된 크기)이 실제로 표시되는 사각형 기준 픽셀로 바꾼다. 보통은 뷰 전체이고, 설정이 화면 크기를 고정하면 `fit_rect`(비율을 유지해 가운데에 맞춘 영역)다. 사각형 밖의 점은 가장자리로 붙는다. 멀티터치는 손가락마다 0부터 빈 번호를 준다.

## 헤드리스 확인

`ATLAN_INPUT_CFG=파일 atlan-session LIBRARY DATA_ROOT`는 바인딩을 읽고 첫 프레임 전에 위치 값 하나와 탭(down, up) 하나를 넣는다. 탭 위치는 `ATLAN_TAP=X,Y`(게스트 화면 픽셀, 기본 400,240)다. 로그의 `input: first … event -> …` 줄과 그 뒤 게스트 동작으로 바인딩을 확인한다.

## 검증 상태

`tests/input_tests.cpp`가 큐 합치기, 바인딩 파싱·거부, NMEA 문장, 인자 워드 배치, 합성 게스트 함수로의 전달을 확인한다. 실제 라이브러리에서 인자 순서와 단위가 맞는지, 실기기에서 CoreLocation과 터치가 동작하는지는 이 테스트로 확인되지 않는다.

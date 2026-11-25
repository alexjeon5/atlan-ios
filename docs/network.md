# 네트워크

게스트는 두 길로 통신한다. 둘 다 호스트의 실제 네트워크로 이어진다.

## 온라인 상태

설정의 `USECONNECTIONMANAGER`가 켜져 있으면(기본값 1) 게스트는 Java 쪽이 온라인 상태 메시지를 보내기 전까지 오프라인으로 동작한다. 원본 Java 코드는 게스트가 `MainActivity.SendMsgFromAtlan`으로 길안내 시작(17)을 알리면 핸들러에서 `nativeReceiveMsgData(REC_ONLINE_STATE = 0x429, 연결 여부, 0, null)`로 답하고, 연결 상태가 바뀔 때마다(`CONNECTIVITY_CHANGE`) 같은 메시지를 다시 보낸다. `shell/java_services.*`가 이를 그대로 따르고, 게스트가 직접 묻는 `MainActivity.IsConnected()`에도 답한다. 연결 여부는 `SessionConfig::network_available`(iOS: Network 프레임워크 경로 감시, 없으면 항상 연결)로 정한다.

## Java 쪽 HTTP

`AtlanNetService.MgrPlusDataUrlConnection(url, timeout, id)`(GET)와 `MgrPlusPostDataUrlConnection(url, body, timeout, id)`(POST, `application/x-protobuf`)는 백그라운드 요청이다. 끝나면 게스트의 정적 네이티브 `AtlanNetService.setResult(byte[] body, int status, int id)`를 부른다. status는 원본과 같다: 성공 1, 실패 0(본문 없음), 잘못된 URL 4. GET은 4xx·5xx를 실패로(HttpURLConnection의 `getInputStream` 예외), POST는 2xx만 성공으로 본다. 제한 시간은 원본처럼 13초가 상한이다. 응답은 큐에 쌓였다가 게스트 스레드에서 입력과 같은 자리에서 전달된다.

요청은 `SessionConfig::http`가 처리한다. 기본값은 `shell/http_client.*`의 평문 http 클라이언트(리다이렉트, chunked 지원)이고, iOS는 https만 NSURLSession으로 보낸다(`platform/apple/apple_network.mm`). 평문 http는 BSD 소켓이라 App Transport Security 예외가 필요 없다.

## 게스트 소켓

게스트 네이티브 코드의 `socket`·`connect`·`send`·`recv`·`setsockopt`·`getsockopt`·`gethostbyname`·`close`는 `src/sockets.cpp`(`GuestSockets`)가 호스트 소켓으로 처리한다. ARM 리눅스 상수와 errno를 호스트 값으로 바꾼다. 소켓 디스크립터는 `0x400`부터라 파일 디스크립터와 겹치지 않고, 그 밖의 `close`는 원래 서비스로 넘긴다.

호스트 소켓은 항상 논블로킹이다. 연결 중이거나 받을 데이터가 없으면 호출한 게스트 스레드만 막고 5ms마다 다시 시도한다(`SO_RCVTIMEO`·`SO_SNDTIMEO`, connect는 최대 20초). 이름 조회는 호스트 스레드에서 하고 1분 캐시한다. IPv6 전용망(NAT64)에서는 connect 때 IPv4 주소를 시스템이 합성한 IPv6 주소로 바꿔 그 주소족의 소켓으로 다시 만든다.

## 로그

연결마다 `net connect 주소:포트 ok|failed|timed out`, `net close … sent=N received=N`, 요청마다 `http GET|POST URL id=…`와 결과 줄이 남는다. URL은 쿼리 문자열을 뺀 형태만 남긴다(키·개인 값이 쿼리에 들어간다). iOS 앱은 이 줄들을 Documents의 `net.log`에도 쓴다.

헤드리스 확인: `ATLAN_INTERACTIVE_SECONDS=60 build/atlan-session "" DATA_ROOT`에서 `java online state connected`와 첫 `http GET … -> HTTP 200` 줄이 나온다. `network-tests`는 루프백 서버로 소켓 서비스와 HTTP 클라이언트를 검사한다.

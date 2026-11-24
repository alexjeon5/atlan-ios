# 오디오 (OpenSL ES)

게스트가 쓰는 OpenSL ES 부분집합을 호스트 서비스로 구현한다. 오디오 장치에 닿는 부분은 `AudioSink` 인터페이스 뒤에 있고, 코어(`opensles`, `audio`)는 C++20 이식 코드다. Apple 백엔드는 `platform/macos/apple_audio_sink.*`(iOS에서도 같은 파일 사용), Linux·테스트용은 `RecordingAudioSink`다.

## 게스트 쪽 모델

`SL_IID_*`는 데이터 import이며 `OpenSles::data_symbol`이 주소를 준다(읽기 전용 페이지의 단어 하나가 IID 블록을 가리킨다). 객체와 인터페이스는 게스트 메모리의 단어이고 읽기 전용 vtable을 가리킨다. vtable 슬롯은 서로 다른 미매핑 trap 주소라서, 구현하지 않은 메서드는 `SLEngineItf::CreateMidiPlayer`처럼 이름과 함께 멈춘다. 호스트 포인터는 게스트에 노출되지 않는다.

구현된 것:
- `slCreateEngine`(옵션은 `SL_ENGINEOPTION_THREADSAFE`만), `SLObjectItf` Realize(동기만)·GetState·GetInterface·Destroy
- `SLEngineItf` CreateOutputMix·CreateAudioPlayer
- `SLPlayItf` SetPlayState·GetPlayState
- `SLBufferQueueItf`·`SLAndroidSimpleBufferQueueItf` Enqueue·Clear·GetState·RegisterCallback
- `SLVolumeItf` SetVolumeLevel·GetMaxVolumeLevel·SetMute

요청했지만 제공하지 않는 optional 인터페이스는 노출하지 않고(GetInterface는 `SL_RESULT_FEATURE_UNSUPPORTED`와 null 포인터), required면 생성이 같은 코드로 실패한다. 형식은 16비트 리틀엔디언 PCM 모노/스테레오, 정수 Hz만 받고 나머지는 명시적으로 멈춘다.

## 완료 콜백

게스트 콜백은 오디오 장치 스레드가 아니라 게스트 스레드에서만 돈다. 임베더가 서비스 밖(최상위)에서 `OpenSles::pump`를 주기적으로 부르면, 장치가 재생을 끝낸 버퍼마다 콜백(`caller`=큐 인터페이스, `context`)을 호출한다. 재생된 양은 `AudioStream::played_bytes()`로 판단한다.

## 관찰과 가정

- 관찰(원본 실행, Linux): nativeInit은 `slCreateEngine`(THREADSAFE 옵션) → Realize → GetInterface(ENGINE) → CreateOutputMix(`ENVIRONMENTALREVERB` optional) → Realize → GetInterface(`ENVIRONMENTALREVERB`)까지 간다. 그 뒤 `MAP/WAVE/*.idx` 네 개를 `fopen`하고(없어도 처리됨) `sem_init`을 거쳐 `pthread_attr_init`에서 멈췄다(스레드 서비스는 [threads.md](threads.md)로 추가, 그 이후 진행은 원본 재실행으로 확인 필요). 오디오 플레이어와 버퍼 큐는 아직 호출되지 않았다.
- 가정(관찰 전, 스펙 기준 구현): 플레이어·버퍼 큐·재생·볼륨 서비스. 원본이 실제로 어떤 형식과 호출을 쓰는지는 스레드 생성이 가능해진 뒤 확인해야 한다.
- 가정: `SetPlayState(STOPPED)`는 일시정지와 같이 다룬다(큐 유지). 볼륨 최대치는 0 dB, 최소 -96 dB.
- Apple 백엔드: 이 환경에서는 컴파일하지 못했다. CI 컴파일과 실기기 청취 확인이 필요하다.

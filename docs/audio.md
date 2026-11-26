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

## 앱이 직접 재생하는 소리 파일

OpenSL ES와 별개로, 앱의 Java 쪽이 미디어 플레이어로 데이터 폴더의 WAV 파일을 직접 재생하는 경우가 있다. 엔진은 `atlan/wav.hpp`의 `parse_wav`(16비트 PCM, `WAVE_FORMAT_EXTENSIBLE` 포함)와 `play_wav_file`(파일 하나를 처음부터 한 번 재생하는 스트림)을 제공하고, 어댑터는 `AppHost::audio()`로 세션과 같은 `AudioSink`를 받아 쓴다. 게스트의 OpenSL ES 출력과 같은 장치·오디오 세션으로 나간다. 어떤 파일을 언제 재생하고 멈추는지는 어댑터가 정하고, 스트림은 `uninstall_java()`에서 닫는다.

## iOS 오디오 세션

세션은 `Playback` 카테고리, `VoicePrompt` 모드로 다른 앱 소리와 섞어서(`MixWithOthers`) 재생한다. 들리는 PCM(약 -54 dBFS 이상의 샘플)이 스트림에 들어오거나 장치로 나가면 그때부터 마지막 소리 뒤 1초까지만 `DuckOthers`(말소리 앱은 `InterruptSpokenAudioAndMixWithOthers`)로 다른 앱 소리를 낮춘다. 게스트가 재생 상태를 켜 둔 채 버퍼를 넣는지와 무관하게 실제 소리로 판단한다. 다른 앱은 세션을 다시 활성화해야 덕킹 변경을 따르므로, 바뀔 때마다 실행 중인 큐를 멈추고 세션을 끄고(덕킹을 풀 때는 다른 앱에 알림) 옵션을 바꿔 다시 켠 뒤 큐를 재개한다. 큐는 버퍼를 그대로 들고 있어서 소리가 빠지지 않는다. 백그라운드에서는 덕킹하지 않는다(정지된 앱은 덕킹을 풀 수 없다). `AppleAudioSink::set_duck_others(false)`면 늘 섞기만 한다. iOS 셸은 런처 설정 > 일반 > 소리 토글을 이 값으로 넘긴다.

## 완료 콜백

게스트 콜백은 오디오 장치 스레드가 아니라 게스트 스레드에서만 돈다. 임베더가 서비스 밖(최상위)에서 `OpenSles::pump`를 주기적으로 부르면, 장치가 재생을 끝낸 버퍼마다 콜백(`caller`=큐 인터페이스, `context`)을 호출한다. 재생된 양은 `AudioStream::played_bytes()`로 판단한다.

## 관찰과 가정

- 관찰(원본 실행, Linux): nativeInit은 `slCreateEngine`(THREADSAFE 옵션) → Realize → GetInterface(ENGINE) → CreateOutputMix(`ENVIRONMENTALREVERB` optional) → Realize → GetInterface(`ENVIRONMENTALREVERB`)까지 간다. 그 뒤 `MAP/WAVE/*.idx` 네 개를 `fopen`하고(없어도 처리됨) `sem_init`을 거쳐 `pthread_attr_init`에서 멈췄다(스레드 서비스는 [threads.md](threads.md)로 추가, 그 이후 진행은 원본 재실행으로 확인 필요). 오디오 플레이어와 버퍼 큐는 아직 호출되지 않았다.
- 가정(관찰 전, 스펙 기준 구현): 플레이어·버퍼 큐·재생·볼륨 서비스. 원본이 실제로 어떤 형식과 호출을 쓰는지는 스레드 생성이 가능해진 뒤 확인해야 한다.
- 가정: `SetPlayState(STOPPED)`는 일시정지와 같이 다룬다(큐 유지). 볼륨 최대치는 0 dB, 최소 -96 dB.
- Apple 백엔드: 이 환경에서는 컴파일하지 못했다. CI 컴파일과 실기기 청취 확인이 필요하다.

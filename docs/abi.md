# 호출 ABI, 로깅, JNI 부트스트랩

## 인자 읽기

`ImportCall::arg(i)`는 r0-r3만 읽는다. 다섯 번째 이후 word는 서비스 진입 시점의 `[sp]`부터 이어지며 `ImportCall::read_arg(i, out)`로 읽는다. 게스트 메모리를 읽을 수 없으면 false다.

`VarArgs`는 AAPCS32 가변 인자를 읽는다. `from_call`은 고정 인자 수 뒤의 r0-r3·스택 word를, `from_va_list`는 `va_list` 값(이 ABI에서는 다음 슬롯을 가리키는 포인터 하나이며 r0-r3로 값 전달된다)을 시작점으로 쓴다. `long long`·`double`은 짝수 슬롯(스택은 8바이트 경계)에서 시작하므로 r3만 남았으면 건너뛴다. 한 번 실패한 리더는 계속 실패 상태다.

`format_guest_printf`는 게스트 포맷 문자열을 호스트에서 전개한다. 플래그 `-+ #0`, 폭·정밀도(숫자 또는 `*`), 길이 `hh h l ll z t j`, 변환 `d i u x X o c s p e E f F g G %`를 지원한다. `%n`, `long double`, 알 수 없는 변환, 읽을 수 없는 인자는 건너뛰지 않고 오류로 멈춘다. 출력·포맷·문자열 길이에 상한이 있다. `%s`의 null은 `(null)`, `%p`는 `0x…`로 쓴다(Bionic 동작은 가정).

## 로그

`AndroidLog::install`은 `__android_log_write`, `__android_log_print`, `__android_log_vprint`를 구현한다. 항목은 호출자가 준 sink로만 전달되고 호스트 파일·syscall로 가지 않는다. null tag는 빈 문자열이다. 반환값은 메시지 길이다(실제 Bionic은 write 결과를 반환한다; 호출자가 무시한다는 것은 가정).

## JavaVM / JNIEnv

`JavaBootstrap`은 읽기 전용 게스트 페이지에 `JavaVM`, `JNIEnv` 객체와 함수 테이블을 만든다. 모든 슬롯은 서로 다른 미매핑 trap 주소를 가리키므로, 구현하지 않은 JNI 함수를 부르면 `JNIEnv::이름`으로 명시적으로 멈춘다. 지금 구현된 것은 관찰된 호출뿐이다.

- `JavaVM::GetEnv`: JNI 1.1/1.2/1.4/1.6만 성공. 그 외 버전은 `*penv = 0`, `JNI_EVERSION`.
- `JNIEnv::FindClass`: 이름을 기록하고 불투명 핸들(미매핑 게스트 주소)을 준다. 클래스 존재 여부는 검증하지 않는다.
- `JNIEnv::GetMethodID` / `GetStaticMethodID` / `GetFieldID` / `GetStaticFieldID`: 클래스 핸들을 확인하고 (종류, 클래스, 이름, 시그니처)를 기록해 불투명 id를 준다. 멤버 존재 여부는 검증하지 않는다.
- `JNIEnv::RegisterNatives`: `JNINativeMethod` 표를 읽어 전부 유효할 때만 기록한다. 게스트 함수 주소는 Thumb 비트를 보존한다.

- `JNIEnv::NewObject` / `NewObjectV` / `NewObjectA`: `()V` 생성자로 어떤 클래스든 **불투명 호스트 객체**(클래스 이름 + 필드 저장소)를 만든다. Java 코드는 실행하지 않으며, 인자가 있는 생성자는 오류로 멈춘다.
- `Get<T>Field` / `Set<T>Field`: 호스트 객체의 필드 저장소를 읽고 쓴다. 쓰지 않은 필드는 0이다. 객체와 필드 id의 클래스가 다르면 오류다. `jlong`/`jdouble`은 r3를 건너뛰어 스택으로 오고 r0:r1로 반환한다.
- `Call<T>Method{,V,A}`와 `CallStatic<T>Method{,V,A}`: `JavaBootstrap::register_method`로 호스트가 등록한 (클래스, 이름, 시그니처)만 실행한다. 인자는 시그니처에 따라 가변 인자·`va_list`·`jvalue[]`에서 읽는다. 등록되지 않았으면 `JNIEnv::CallVoidMethod: no host implementation for …`처럼 이름을 붙여 멈춘다. `CallNonvirtual*`은 아직 없다.
- `JNIEnv::NewGlobalRef` / `DeleteGlobalRef`: 클래스·객체 핸들에 대한 별도 전역 참조. 위조·이중 삭제는 오류다.
- `JNIEnv::DeleteLocalRef`: 핸들만 검증한다. 클래스·객체 테이블은 회수하지 않는다(상한 있음).

문자열 내용, 배열, 예외 등 나머지 Java 객체 모델은 없다. 관찰된 호출이 필요로 할 때만 하나씩 추가한다.

## 추가된 JNI 동작

- `GetVersion`은 JNI 1.6을 반환한다. 호스트 코드는 예외를 던지지 않으므로(`Throw`/`ThrowNew`는 이름으로 멈춤) `ExceptionCheck`·`ExceptionOccurred`는 항상 없음, `ExceptionClear`는 변화 없음이다.
- `GetObjectClass`는 호스트 객체의 클래스 핸들을 준다.
- `CallNonvirtual<T>Method{,V,A}`는 `(env, obj, clazz, id, …)` 순서로 읽고 `clazz`가 메서드의 클래스와 같아야 한다. 이후는 일반 호출과 같다(등록된 호스트 메서드만 실행).
- `byte[]`만 모델링한다: `NewByteArray`, `GetArrayLength`, `Get/SetByteArrayRegion`(범위 밖은 오류), `Get/ReleaseByteArrayElements`(스크래치 복사본, 모드 0 복사 후 해제·1 복사만·2 버림). 호스트에서는 `new_byte_array`/`byte_array`로 만들고 읽는다. 다른 배열 타입과 객체 배열은 이름으로 멈춘다.
- 문자열은 `NewStringUTF`, `GetStringUTFChars`/`Release`, `GetStringUTFLength`, 호스트의 `new_string`/`string_text`를 쓴다(UTF-8, 복사본은 스크래치 메모리).

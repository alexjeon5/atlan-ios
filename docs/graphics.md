# 그래픽: GLES2 → Metal

게스트의 GLES2/EGL 호출은 `src/gles.cpp`가 인자를 호스트 값(`GlCall`)으로 풀어 `GlBackend`에 넘긴다. 실제 렌더링은 세 층으로 나뉜다.

| 층 | 파일 | 역할 |
| --- | --- | --- |
| 번역기 | `src/glsl.cpp` | GLSL ES 1.00 소스를 MSL로 바꾸고, uniform 블록 배치와 attribute/sampler 슬롯을 계산한다 |
| 상태기계 | `src/gl_context.cpp` | 셰이더·프로그램·버퍼·텍스처와 고정 상태를 추적하고, draw 호출을 자체 완결된 `GlDraw` 패킷으로 만든다 |
| 싱크 | `platform/apple/metal_gl_backend.mm` | `GlDraw`/clear/present를 Metal로 실행한다 (테스트에서는 기록기) |

번역기와 상태기계는 플랫폼 독립 C++20이라 Linux에서 단위 테스트된다. Metal 코드만 Apple 전용이다.

Android에서는 이 세 층을 쓰지 않는다. `platform/gles/gles_backend.*`가 `GlCall`을 기기의 GLES2에 그대로 넘기고(셰이더 원문, 압축 텍스처 포함), 게스트의 기본 프레임버퍼를 오프스크린 프레임버퍼로 두어 `eglSwapBuffers`마다 창에 표시한다. 같은 코드가 Linux에서 Mesa로 돈다: `gles-backend-tests`, 그리고 `ATLAN_GLES=DIR atlan-session …`(실제 게스트를 기기 경로로 그려 `DIR/gles_NNNNN.ppm`으로 저장). [Android](android.md)

## 셰이더 번역

- 입력은 앱 데이터의 GLSL ES 평문이다. 셰이더 하나씩 `compile()`로 파싱·타입 검사하고(`glCompileShader` 상태와 info log가 여기서 나온다), `glLinkProgram`에서 `link()`가 정점/조각 쌍을 하나의 MSL 라이브러리로 합친다.
- 타입 인식 번역이다. 벡터 생성자 평탄화(`vec4(vec3, vec4)`의 남는 인자 버림 포함), `vec3(vec4)` 절단, 행렬·벡터 `==`, `mat4(1.0)`, `mod`, `texture2D*`, 스위즐(`stpq`→`xyzw`), 스칼라→벡터 승격, int→float 관대 변환을 처리한다.
- 전역 변수는 MSL에 없으므로 모든 helper 함수는 숨은 인자(`u`, 단계별 입출력 구조체, 샘플러 쌍)를 받는다.
- 클립 공간 깊이는 GL의 [-1, 1]에서 Metal의 [0, 1]로 정점 셰이더 끝에서 변환한다.
- uniform 블록의 모든 항목은 16바이트 경계에서 시작한다. 호스트 쪽 배치가 MSL 패딩 규칙에 의존하지 않는다. 위치(location)는 배열 원소마다 하나씩 연속으로 매기고, sampler는 그 뒤에 온다.
- 지원하지 않는 것은 컴파일 오류로 보고한다: struct, 전역 비-const 변수, samplerCube, 미분 함수, `gl_FragCoord`/`gl_PointCoord`, 정점 텍스처 조회.

## 상태기계와 draw 패킷

- attribute는 draw 시점에 float로 변환되고 셰이더의 attribute 폭으로 넓혀진다(빠진 성분은 GL처럼 0, 0, 0, 1). 정규화 정수형, VBO, 클라이언트 배열, 비활성 attribute(현재 값 0,0,0,1)를 모두 같은 경로로 처리한다.
- `GL_LINE_LOOP`와 `GL_TRIANGLE_FAN`은 인덱스로 펼친다. 모든 draw는 인덱스 draw다.
- 텍스처는 RGBA8로 변환한 불변 스냅샷이다(RGB/LUMINANCE/ALPHA/565/4444/5551, `UNPACK_ALIGNMENT` 반영). 레벨별로 보관하고 완전한 밉맵 체인만 샘플링에 쓴다. 압축 텍스처는 아직 없다.
- 알 수 없는 location(-1)에 대한 `glUniform*`/`glEnableVertexAttribArray`는 GL처럼 조용히 무시한다.
- 게스트의 `renderGLSF`는 `eglSwapBuffers`를 호출하지 않는다(GLSurfaceView가 대신 한다). 세션이 `renderGLSF`가 끝날 때마다 `swap_buffers`를 보낸다.

## Metal 싱크

- MSL은 실행 시 `newLibraryWithSource`로 컴파일한다. iOS에서 허용되는 경로다(컴파일은 시스템 컴파일러 서비스가 하고 앱 프로세스에 쓰기+실행 메모리를 만들지 않으므로 JIT 금지와 무관하다). 프로그램당 한 번 컴파일하고 프로그램·블렌드 조합별 파이프라인을 캐시한다.
- 프레임 하나는 command buffer 하나다. `glClear`는 새 render pass의 load action이 되고, 프레임 끝(`present`)에서 commit한다.
- draw마다 새 `MTLBuffer`를 만들지 않는다. 인덱스와 4 KiB가 넘는 정점·uniform 데이터는 프레임 슬롯 세 개가 돌려 쓰는 공유 버퍼에 복사하고, 슬롯은 그것을 마지막으로 쓴 command buffer가 끝난 뒤에 다시 쓴다. 정점이 65,535개 이하면 16비트 인덱스를 쓴다(스트립 재시작 값 0xFFFF가 나올 수 없다). 인코더에 이미 설정된 파이프라인·깊이 상태·컬링·감김·viewport·텍스처·샘플러는 다시 보내지 않는다.
- `GlContext`는 draw 패킷 하나를 재사용해 draw마다 벡터를 새로 할당하지 않는다.
- GL 좌표 규약은 Metal과 일치한다. 감김(`glFrontFace`)은 그대로 쓰고, viewport의 y만 하단 원점에서 상단 원점으로 바꾼다. macOS CI의 렌더 테스트가 이를 픽셀로 검증한다.
- iOS 셸은 게스트가 `setSurfaces(w, h)`로 알려 준 크기로 레이어의 `drawableSize`를 맞추고, 화면에는 항상 뷰 전체로 늘려 보여 준다.

## 아직 없는 것

`glLineWidth`(Metal에 선 굵기가 없다), 스텐실, scissor, FBO, ETC1/PVRTC 압축 텍스처, `glGetActiveUniform` 계열. 게스트가 쓰는 import 57개 중 위 항목에 해당하는 것은 `glLineWidth`, `glCompressedTexImage2D`뿐이다.

`glReadPixels`는 기본 프레임버퍼를 RGBA8로 읽는다. 그 시점까지 그린 내용을 커밋해 읽을 수 있는 텍스처로 복사하고 프레임은 새 커맨드 버퍼로 이어 간다(그래서 레이어는 `framebufferOnly`가 아니다). 게스트는 화면 전환 애니메이션에서 이전·다음 화면을 이렇게 읽어 텍스처로 올린 뒤 밀어서 그리므로, 읽기가 비어 있으면 전환 동안 검은 화면이 나온다.

## 구현 범위

GL 기능은 게스트가 실제로 부르는 것만 구현하고, 나머지는 관측될 때까지 미룬다. 실제 호출은 `atlan-session`의 `ATLAN_RAW_GL=1` 기록으로 확인한다(기록 결과는 공개 문서에 남기지 않는다).

- draw는 삼각형 계열(`GL_TRIANGLES`, `GL_TRIANGLE_STRIP`)을 지원한다. 인덱스는 ushort/ubyte, attribute는 float/byte/short/ubyte, VBO를 지원한다.
- 텍스처는 RGBA/UNSIGNED_BYTE와 밉맵 체인을 지원한다. 레벨별로 보관하고, 레벨 0부터 1x1까지 크기가 맞는 완전한 체인이면 Metal 밉 텍스처로 올리며, 밉맵 필터인데 체인이 불완전하면 GL처럼 검게 읽힌다.
- 넓은 선(`glLineWidth`), 압축 텍스처, 스텐실·scissor·FBO·`glBlendEquation` 계열은 아직 구현하지 않았다(넓은 선은 쿼드 확장으로 할 계획).
- 인덱스가 element 버퍼에 있고 attribute가 클라이언트 배열인 draw(지도의 3D 랜드마크, 현재 위치 표시 등)는 서비스(`src/gles.cpp`)가 element 버퍼에 올린 내용을 기억해 가장 큰 인덱스까지 정점을 모은다. 전에는 정점이 모이지 않아 이런 draw가 보이지 않았다.

## 검증

- `glsl-tests`, `gl-context-tests`: 번역 결과와 GL 상태 처리(Linux, 모든 CI 러너).
- `glsl-mock-metal`: 번역된 MSL을 `tests/mock_metal`의 가짜 `<metal_stdlib>`로 C++로 컴파일해 실제로 실행하고 값을 확인한다(Clang 전용).
- `metal-render`: 실제 Metal로 오프스크린에 그려 픽셀을 확인한다(macOS 러너, GPU가 없으면 건너뜀). `ATLAN_SHADER_DAT=<파일>`을 주면 그 컨테이너의 모든 프로그램이 Metal 컴파일러와 파이프라인 생성을 통과하는지도 확인한다.
- `atlan-session LIBRARY DATA_ROOT [TRACE]`: 게스트를 헤드리스로 돌려 상태기계가 받은 clear/draw/present를 기록한다. `ATLAN_RAW_GL=1`이면 디코드된 GL 호출 원본을 기록한다.
- `atlan-glsl VERT FRAG`: 한 쌍을 번역해 MSL을 출력한다.

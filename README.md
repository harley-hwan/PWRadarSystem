# PWRadarSystem

해상·공중 탐지용 PW(Pulsed Waveform) 레이다 탐지 시스템.
**순수 C17**, **외부 라이브러리 의존성 0**, **Windows / Linux 크로스플랫폼**.

| 프로젝트 | 산출물 | 역할 |
|---|---|---|
| `PWRadarCore` | `PWRadarCore.dll` / `libPWRadarCore.so` | 신호처리 엔진 (시뮬레이터 → 펄스압축 → MTI → Doppler → CFAR → 클러스터링 → 트래커) |
| `PWRadarUI`   | `PWRadarUI.exe` / `PWRadarUI`           | 실시간 탐지 검증 콘솔. MATLAB 스타일 플로팅을 자체 구현 |

의존성은 OS가 이미 제공하는 것뿐입니다 — Windows는 `user32` + `gdi32`,
Linux는 `libX11` + `pthread` + `libm`. 수치 라이브러리도, GUI 툴킷도,
폰트 라이브러리도 쓰지 않습니다. FFT·난수·칼만 필터·안티에일리어싱 렌더러·
폰트 래스터라이저까지 전부 이 저장소 안에 있습니다.

---

## 1. 빌드

**CMake가 빌드의 단일 소스입니다.** Windows에서도 `.sln`/`.vcxproj`를 저장소에
넣어두지 않고 CMake가 **생성**합니다 — 프로젝트 XML을 손으로 쓰면 검증할 수가
없고, 실제로 그것이 "One or more projects in the solution were not loaded
correctly" 의 원인이었습니다. Visual Studio 제너레이터가 만든 솔루션은 MSBuild
자신이 쓴 것이므로 반드시 열립니다.

C 프로젝트에 CMake를 쓰는 건 표준입니다 — CMake는 언어 중립이고, C 전용 기능은
따로 필요하지 않습니다.

### Windows

```bat
build.bat                :: Release x64  → 구성 + 빌드 + 수치 검증
build.bat Debug          :: Debug
build.bat Release run     :: 빌드 후 콘솔 실행
build.bat clean           :: build 디렉터리 삭제
```

빌드가 끝나면 IDE로 작업하실 수 있습니다:

```
build\PWRadarSystem.sln 열기   ← CMake가 생성. F5로 바로 실행됩니다
```

`build.bat`은 CMake를 PATH → Visual Studio 2022 번들 → 독립 설치 순으로 직접
찾으므로 별도 설정이 필요 없습니다. 필요 조건은 Visual Studio 2022 +
**"C++를 사용한 데스크톱 개발"** 워크로드 하나뿐입니다.

산출물: `build\Release\PWRadarUI.exe`, `build\Release\PWRadarCore.dll`

### Linux

```sh
./build.sh                # Release → 구성 + 빌드 + 수치 검증
./build.sh Debug
./build.sh Release run
./build.sh clean
```

산출물: `build/PWRadarUI`, `build/libPWRadarCore.so`
(설치 필요: `sudo apt install build-essential cmake libx11-dev`)

### 수동 CMake 호출

```sh
# Windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\PWRadarUI.exe --selftest

# Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/PWRadarUI --selftest
```

CMake 없이 Linux에서 바로 빌드하려면 `make -j && make test` 도 됩니다.

### 경고 정책

GCC/Clang 쪽은 다음 수준에서 **경고 0개**로 검증되었습니다:

```
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
-Wmissing-prototypes -Wcast-qual -Wpointer-arith -Wwrite-strings
```

MSVC `/W4`는 진단 항목이 일부 다르고 이 환경에서 검증할 수 없었기 때문에
**경고를 에러로 취급하는 옵션은 기본 해제**입니다. 트리가 깨끗한 것을
확인하신 뒤 켜세요:

```sh
cmake -S . -B build -DPWR_WERROR=ON
```

### Windows 코드 경로 교차 검증

Linux에서 MSVC 없이 Win32 계층 전체를 컴파일·링크해 볼 수 있습니다:

```sh
sudo apt install mingw-w64
./tools/check_windows_build.sh
```

이 스크립트는 세 개의 게이트를 순서대로 통과시킵니다.

1. **이름 충돌 게이트** — `tools/check_name_collisions.py` 가 공개 헤더의 모든
   식별자(489개)를 Windows SDK 헤더 전체의 오브젝트형 매크로(13만여 개)와
   대조합니다. 하나라도 겹치면 여기서 빌드가 멈춥니다. 아래 §1.1 참조.
2. **컴파일·링크** — `-Werror` 로 `PWRadarCore.dll`(pwr_* 심볼 60개 export) +
   `PWRadarUI.exe` 를 만듭니다. 창 프로시저·DIB 프레임버퍼·Win32
   스레딩/원자연산·dllexport/dllimport 배선까지 전부 지나갑니다.
3. **실행** — wine이 설치되어 있으면 방금 만든 `PWRadarUI.exe --selftest` 를
   **실제로 실행**합니다. "컴파일된다"와 "동작한다"는 다른 주장이기 때문입니다.
   §1.1의 버그는 `-Werror` 를 무경고로 통과하고 런타임에만 나타났습니다.

검증 상태: Windows 경로는 컴파일·링크(무경고) → `--selftest` 9/9 → Xvfb 위에서
GUI 40프레임 렌더까지 wine으로 **실행 확인** 완료. Linux 경로는 컴파일·링크·
실행·수치 검증 9/9·CTest·렌더링 확인 완료. 남은 미확인 영역은 **MSVC 고유
진단**과 **실제 Windows 커널에서의 실행**뿐입니다(wine은 Windows가 아니고
mingw는 MSVC가 아닙니다).

### 1.1 `PWR_OK` 를 쓰지 않는 이유 (실제로 겪은 버그)

`PWRadarUI.exe` 가 `engine creation failed: thread failure` 로 죽는 버그가
있었습니다. 원인은 스레딩이 아니라 **이름 충돌**입니다. `<winuser.h>` 는
레거시 `WM_POWER` 브로드캐스트 상수를 오브젝트형 매크로로 정의합니다:

```c
#define PWR_OK              1        /* winuser.h */
#define PWR_FAIL            (-1)
#define PWR_SUSPENDREQUEST  1
#define PWR_CRITICALRESUME  3
```

매크로는 스코프와 무관하게 열거자를 이깁니다. 따라서 `<windows.h>` 에 닿는
번역 단위(`pwr_platform.c`)에서 `PWR_OK = 0` 열거자가 전처리기에 의해 `1` 로
치환되고, `return PWR_OK;` 는 `return 1;` 이 됩니다. `pwr_mutex_init` /
`pwr_cond_init` / `pwr_thread_create` 는 전부 성공했는데 호출자의
`status != PWR_OK` 검사가 성사되어 `pwr_engine_create` 가 `PWR_ERR_THREAD` 를
반환한 것입니다. 증상이 원인에서 멀고, `-Werror` 를 무경고 통과하며,
**Linux 빌드는 아무 영향이 없습니다.**

대응은 세 겹입니다.

| 방어선 | 위치 | 성질 |
|---|---|---|
| 이름을 아예 쓰지 않음 | `PWR_STATUS_OK` 로 개명 | 근본 해결 |
| 컴파일 타임 가드 | `PWRadarCore/src/pwr_guard.c` | `<windows.h>` 를 **먼저** 인클루드하는 전용 TU에서 `#if defined(...)` + `_Static_assert` |
| 빌드 게이트 | `tools/check_name_collisions.py` | 인클루드 순서와 무관하게 SDK 전체 스윕 |

`#undef PWR_OK` 는 해결이 아닙니다. 그 매크로를 정당하게 필요로 하는 소비자가
있고, 인클루드 순서가 바뀌는 순간 충돌이 되돌아옵니다.

주의할 점 하나: **`pwr_status.h` 안에 `#if defined(PWR_OK)` 트립와이어를 두면
잡히지 않습니다.** 실제 번역 단위는 공개 헤더를 `<windows.h>` 보다 *먼저*
인클루드하므로, 트립와이어가 평가되는 시점에는 문제의 매크로가 아직 존재하지
않습니다. 그래서 가드가 인클루드 순서를 우리가 통제하는 `pwr_guard.c` 에
있습니다. 이 가드와 게이트는 둘 다 결함을 되돌려 넣어 **실제로 실패하는 것을
확인**했습니다.

### 명령행 옵션

```
--selftest             코어 수치 검증 스위트 실행 후 종료 (CI 게이트용, 실패 시 non-zero)
--scenario N           시작 시 N번 시나리오 로드
--list-scenarios       시나리오 목록 출력
--capture N FILE       N 프레임 렌더 후 프레임버퍼를 PPM으로 저장하고 종료 (헤드리스 회귀 검증)
--version / --help
```

---

## 2. 설계에서 반드시 알아야 할 것

### 2.1 모든 dB 값은 SNR로 캘리브레이션되어 있습니다

시뮬레이터는 복소 샘플당 열잡음 분산을 **정확히 1.0** 으로 주입하고,
신호처리 체인이 그 기준을 끝까지 보존합니다. 결과적으로

> **Range-Doppler 맵, A-scope, RTI 워터폴에 표시되는 dB 값은 곧 SNR입니다.
> 잡음 바닥이 0 dB에 놓입니다.**

화면에서 18 dB로 읽히는 표적은 실제로 적분 후 SNR 18 dB를 가집니다.
따라서 임계값·탐지·트랙 품질 수치에 추가 스케일링이 필요 없고,
운용자는 디스플레이를 그대로 SNR 계측기로 읽을 수 있습니다.

정규화 상수는 가정이 아니라 **측정**됩니다 (`pwr_waveform.c`):
압축 필터를 단위 피크 이득으로 정규화한 뒤, 단위 분산 백색 입력에 대한
실제 진폭 이득(`noise_gain`)을 파스발 관계로 계산해 `sigma_pc`로 씁니다.

### 2.2 펄스압축은 시간영역 테이퍼가 아니라 스펙트럼 등화를 씁니다

```
H(f) = conj(TX(f)) · W(f) / ( |TX(f)|² + ε )
```

LFM 레플리카에 시간영역 창을 곱하는 흔한 구현은 시간↔주파수 정상위상
근사에 의존하고, 그 오차는 `1/sqrt(Tp·B)` 로 줄어듭니다. 실제 감시
레이다가 쓰는 시간대역폭적 100 정도에서는 LFM 스펙트럼의 Fresnel 리플이
**±Tp 전 구간에 −40 dBc 수준의 평탄한 페어드 에코 대(pedestal)** 를 남기고,
이것은 큰 선박 양쪽 ±2.9 km에 유령 탐지를 만들 만큼 강합니다.
`|TX(f)|` 를 등화하면 선택한 테이퍼가 약속한 부엽 레벨이 실제로 나옵니다.

측정된 결과 (정합손실은 전부 0.05 dB 이하):

| 거리 테이퍼 | 달성 PSL | 주엽 폭 | 정합손실 |
|---|---|---|---|
| Rectangular | −25.4 dB | 2.0 bin | 0.42 dB |
| Taylor −35 dB | **−37.4 dB** | 2.0 bin | 0.05 dB |
| Taylor −50 dB | −47.9 dB | 2.0 bin | 0.02 dB |
| Hamming | −43.7 dB | 2.0 bin | 0.02 dB |
| Blackman | −64.5 dB | 4.0 bin | 0.01 dB |
| Chebyshev −60 dB | −64.4 dB | 4.0 bin | 0.01 dB |

### 2.3 Pfa는 셀 개수가 아니라 셀 **발생률**로 정합니다

기본 형상은 CPI당 1000 × 64 셀을 검사하고 1회전당 약 234 CPI를 돌므로
회전당 1.5e7 셀입니다. "스캔당 오경보 1개" 를 목표로 하면 Pfa ≈ 1e-7 이
되고, 습관적으로 쓰이는 1e-6은 스캔당 오경보 15개를 만들어 트랙 파일을
tentative 트랙으로 덮어버립니다. 기본값은 **1e-7** 이며, 제어 패널은
셀당 Pfa와 **스캔당 설계 플롯 수**를 함께 표시합니다.

### 2.4 회전 안테나 트랙 관리

빔이 표적을 비추는 동안에만 트랙이 히트/미스를 적립합니다. 매 CPI마다
모든 트랙을 예측하되, 예측 방위가 조사 부채각 안에 있는 트랙만 연관에
참여합니다. 여기에 **스캔 기반 노후화 규칙**을 더했습니다 — M-of-N
카운터만으로는 빔이 영구히 떠나버린 트랙(예: 단발 오경보로 생긴 트랙)을
결코 회수할 수 없기 때문입니다.

### 2.5 분산 클러터는 압축 영역에 주입합니다

해면/체적 클러터는 송신 펄스 에코의 조밀한 중첩입니다. 원시 I/Q에
**변조되지 않은** 난수열을 더하면 두 번 틀립니다 — 정합필터가 이를
압축하지 않고 펄스 길이 전체로 번지게 하고, 레벨도 압축이득만큼 낮게
나옵니다. 반사율 필드를 처프와 컨볼루션하면 정확하지만 펄스당 고속시간
FFT 2회가 추가로 듭니다. 압축이 선형이고 표면 반사율이 백색이므로,
압축 후 클러터의 통계는 압축 데이터에 직접 더한 백색 필드와 (압축
펄스폭 상관거리 이내에서) 동일합니다. 그래서 펄스압축 직후·MTI 직전에
주입합니다.

### 2.6 프레임 발행

3중 버퍼입니다. 생산자는 항상 "최신 발행 슬롯도 아니고 소비자가 잡고
있는 슬롯도 아닌" 슬롯에 씁니다 — 3개면 그런 슬롯이 항상 존재하므로
생산자는 절대 블록되지 않고 소비자는 절대 찢어진 프레임을 보지 않습니다.

---

## 3. 콘솔 화면 구성

```
┌──────────────────────────────────────────────────────────────────────┐
│ 툴바: RUN/PAUSE/STEP/+1 SCAN/RESET · 시나리오 · 속도 · CPI/s · 부하   │
├─────────┬───────────────────────────────┬────────────────────────────┤
│ 제어    │  PPI 극좌표 스코프  │ R-D 맵  │  트랙 테이블 (정렬 가능)   │
│ 패널    ├─────────────────────┼─────────┤                            │
│ (6탭)   │  A-scope            │ RTI /   │  선택 트랙 / 시스템 판독   │
│         │                     │ 스펙트럼│                            │
├─────────┴───────────────────────────────┴────────────────────────────┤
│ 상태바: 커서 판독 · 단계별 타이밍 · 부하율                            │
└──────────────────────────────────────────────────────────────────────┘
```

모든 경계는 드래그 가능한 스플리터이고, 모든 디스플레이는 독립적인
줌·팬·데이터 커서를 가집니다.

### MATLAB 대응표

| MATLAB | 이 구현 |
|---|---|
| `axes`, `xlim`, `ylim`, `xlabel`, `ylabel`, `title` | `UI_Axes` 필드 |
| `plot`, `stairs`, `area`, `scatter` | `ui_plot_line`, `ui_plot_stairs`, `ui_plot_area`, `ui_plot_scatter` |
| `semilogx`, `semilogy` | `UI_Axes::xlog`, `ylog` |
| `grid on`, `grid minor` | `UI_Axes::grid` (0/1/2) |
| `legend` | `ui_legend` |
| `imagesc` + `colormap` + `caxis` + `colorbar` | `ui_imagesc`, `ui_colormap`, `ui_colorbar` |
| `datacursormode` | `UI_Axes::cursor_*` (우클릭으로 고정) |
| `zoom`, `pan`, `axis tight`, `axis equal` | `ui_axes_input`, `UI_Axes::equal_aspect` |
| `uitable` | `ui_table` (헤더 클릭 정렬, 스크롤, 행 선택) |
| `uicontrol` 계열 | 버튼·토글·체크박스·라디오·슬라이더(선형/로그)·숫자입력·드롭다운·탭·스크롤바·스플리터 |
| `colormap` 이름 | parula, jet, turbo, viridis, hot, gray, bone, cool + phosphor / amber (레이다 스코프용) |

틱은 1-2-5 알고리즘(2.5 포함 — dB축에서 2.5 dB 간격이 훨씬 잘 읽힙니다)으로
자동 생성되고, 트레이스는 목적지 컬럼당 min/max 데시메이션을 하므로
창이 좁아도 부엽 스파이크가 사라지지 않습니다 (오실로스코프와 같은 방식).

### 조작

| 입력 | 동작 |
|---|---|
| 휠 | 포인터 중심 줌 (`Ctrl`+휠 = x축만, `Shift`+휠 = y축만) |
| 좌 드래그 | 사각 영역 줌 |
| 중 드래그 / `Shift`+좌 드래그 | 팬 |
| 더블클릭 | 전체 범위로 복귀 |
| 우클릭 | 데이터 커서 고정/해제 |
| PPI 휠 / 좌클릭 / 더블클릭 | 거리 스케일 / 트랙 선택 / 전체 스케일 |
| `Space` `S` `A` `R` | 실행·정지 / 1 CPI / 1 스캔 / 리셋 |
| `T` `G` | 실제값 오버레이 / 공분산 타원 |
| `1`–`6` `F1` | 제어 패널 탭 / 도움말 |

---

## 4. 검증 시나리오

`--list-scenarios` 또는 툴바 드롭다운:

| # | 시나리오 | 검증 대상 |
|---|---|---|
| 0 | Noise only | 설계 Pfa 대비 실제 오경보율 |
| 1 | Single inbound aircraft (SW1) | 기본 탐지·트랙 개시 |
| 2 | Maritime picture in sea clutter | MTI, R⁻³ 클러터 법칙, 클러터 무릎 근방 표면표적 |
| 3 | Mixed air and surface picture | 정상 운용 상태 |
| 4 | Resolution pair (range + Doppler) | 거리 분해능, OS-CFAR 다표적 |
| 5 | Crossing tracks | 데이터 연관 (동일 셀 통과) |
| 6 | High-speed inbound | Doppler 모호성 폴딩, ambiguous 플래그 |
| 7 | Rain plus noise jammer | CFAR 클러터 경계, 재밍 스트로브 |
| 8 | Detection-range walk | 레이다 방정식 대비 실측 탐지거리 |

---

## 5. 내장 수치 검증 스위트

`PWRadarUI --selftest` (CTest에도 등록되어 있음):

```
T1 FFT vs direct DFT            fwd rel 1.10e-07, round trip 1.26e-07
T2 amplitude tapers             9 tapers checked
T3 sort and quickselect         97 samples
T4 global nearest neighbour     optimum 6, got 6.0
T5 tracker linear algebra       2x2 inverse, gate, symmetrise
T6 pulse compression range      truth 12000 m, peak 11992 m
T7 Doppler axis calibration     truth -35.0 m/s, peak -34.6 m/s
T8 CFAR false-alarm rate        design 1.0e-04, achieved 1.19e-04 over 614400 cells
T9 end-to-end tracking          1 confirmed, best |dy| = 293 m
9/9 cases passed
```

T4는 greedy가 8을 고르는 3×4 문제를 씁니다 — 증강경로 탐색이 실제로
동작하는지 확인하기 위한 것입니다.

---

## 6. 기본 형상과 실측 성능

S밴드 연안 감시 레이다:

| 항목 | 값 |
|---|---|
| 반송파 / 대역폭 / 펄스폭 | 3.05 GHz / 5 MHz / 20 µs (TB = 100, 압축이득 20 dB) |
| 표본율 / PRF | 6.25 MHz / 3000 Hz |
| 거리 분해능 / 셀 간격 | 30.0 m / 24.0 m |
| 비모호 거리 / 속도 | 50.0 km / ±73.7 m/s |
| 속도 분해능 | 4.61 m/s |
| CPI / 스캔 주기 | 32 펄스, 10.67 ms / 2.5 s (24 rpm, 방위 빔폭 1.6°) |
| 처리 격자 | 1000 거리 빈 × 64 Doppler 빈 |
| 안테나 이득 | 29 dBi (1.6°×20° 개구가 실제로 내는 값 — 링크 예산 자체 일관성) |
| 잡음 전력 | −103.0 dBm |
| 탐지거리 (13 dB SNR) | 0.1 m² 18.7 km / 1 m² 33.2 km / 100 m² 105 km |

2-vCPU 클라우드 VM에서 측정된 단계별 시간 (EWMA, ms):

```
simulate 1.8  compress 4.2  MTI 0.0  Doppler 1.1  CFAR 2.8  cluster 0.05  track 0.01
total 11.0 ms  /  예산 10.67 ms  →  부하율 1.03,  91 CPI/s
```

실제 개발 워크스테이션에서는 여유가 크게 남습니다. 부하율이 1을 넘으면
운용자가 `speed` 슬라이더(0.05×–8×)로 슬로모션을 걸 수 있고, 시나리오
시간은 벽시계와 분리되어 있으므로 그림은 언제나 물리적으로 일관됩니다.

---

## 7. 소스 구성

```
PWRadarSystem/
├── CMakeLists.txt                   빌드의 단일 소스 (Windows/Linux 공통)
├── build.bat                        Windows: .sln 생성 + 빌드 + 수치 검증
├── build.sh                         Linux:   구성 + 빌드 + 수치 검증
├── Makefile                         CMake 없이 쓰는 Linux 대안
├── PWRadarCore/
│   ├── include/pwradar/             공개 C ABI (이것만 DLL 경계를 넘습니다)
│   │   ├── pwr_api.h                내보낸 함수 전체
│   │   ├── pwr_types.h              ABI 구조체 (ABI 버전에 종속)
│   │   ├── pwr_status.h  pwr_version.h
│   │
│   └── src/
│       ├── pwr_platform.[ch]        스레드·뮤텍스·원자연산·단조시계·정렬할당
│       ├── pwr_math.[ch]            복소수·테이퍼·PCG32·순서통계량·2×2 선형대수
│       ├── pwr_fft.[ch]             radix-2/4 FFT (혼합기수 정확성 증명 주석 포함)
│       ├── pwr_waveform.c           LFM 처프 + 스펙트럼 등화 압축 필터
│       ├── pwr_sim.c                수신기 프런트엔드 (표적·클러터·재머·다중경로)
│       ├── pwr_chain.c              펄스압축 → MTI → Doppler → 표시물
│       ├── pwr_cfar.c               CA/GO/SO/OS/TM-CFAR + 피크선택 + 클러스터링
│       ├── pwr_track.c              칼만 + 게이팅 + Jonker-Volgenant 할당
│       ├── pwr_engine.c             수명주기·버퍼·워커 스레드·프레임 발행
│       ├── pwr_config.c             기본값·검증·클램프·파생지표·링크예산
│       ├── pwr_scenario.c           검증 시나리오 9종
│       ├── pwr_selftest.c           수치 검증 스위트
│       ├── pwr_guard.c              컴파일 타임 가드 전용 TU (코드 생성 없음).
│       │                            <windows.h> 를 먼저 인클루드해 이름 충돌을
│       │                            잡고, 상태코드 ABI 값과 PWR_Complex 레이아웃을
│       │                            _Static_assert 로 고정합니다. §1.1 참조
│       └── pwr_core.h  pwr_names.c
├── PWRadarUI/src/
│   ├── ui_platform.h                OS 추상화 (창 1개 + 입력 + 블릿, 전부)
│   ├── ui_platform_win32.c          user32 + gdi32 (DIB 섹션 → BitBlt)
│   ├── ui_platform_x11.c            libX11 (XImage → XPutImage)
│   ├── ui_gfx.[ch]                  소프트웨어 렌더러 (AA 라인·폴리곤·원·텍스트·필드 블릿)
│   ├── ui_font.h / ui_font_data.c   임베디드 AA 글리프 아틀라스 (생성물, 57 KB)
│   ├── ui_colormap.[ch]             컬러맵 10종
│   ├── ui_theme.h                   색·치수 단일 정의처
│   ├── ui_widget.[ch]               컨트롤 세트 + uitable + 스플리터
│   ├── ui_plot.[ch]                 MATLAB 대응 axes / 시리즈 / imagesc / colorbar
│   ├── ui_ppi.[ch]                  PPI 스코프 (극좌표 LUT 캐시, 심볼로지)
│   ├── ui_app.[ch]                  레이아웃·제어패널·4개 디스플레이·코어 바인딩
│   └── main.c
└── tools/
    ├── gen_font.py                  폰트 아틀라스 생성기 (빌드에는 관여하지 않음)
    ├── check_name_collisions.py     공개 식별자 vs Windows SDK 매크로 스윕 (§1.1)
    └── check_windows_build.sh       mingw-w64 교차 검증 + wine 실행 게이트
```

폰트 아틀라스는 오프라인에서 생성해 커밋해 둔 것입니다. 덕분에 콘솔은
제대로 안티에일리어싱된 텍스트를 그리면서도 C 런타임 외에 아무것도
링크하지 않습니다 — "외부 라이브러리 금지" 가 UI 계층에서 의미하는 바를
그대로 지킨 것입니다. 페이스·크기·글리프 집합을 바꿀 때만
`python3 tools/gen_font.py PWRadarUI/src/ui_font_data.c` 를 다시 돌립니다.

---

## 8. ABI 규약

`pwr_types.h`의 구조체 배치는 `PWR_ABI_VERSION`에 종속됩니다.
`PWRadarUI`는 시작 시 `pwr_abi_version()`을 자신이 컴파일된 값과 비교하고
불일치하면 실행을 거부합니다. 유지보수 규칙:

* 멤버는 정렬이 큰 것부터 — MSVC x64와 GCC/Clang x86-64에서 배치가 동일해야 합니다
* 비트필드 금지, `bool` 금지, enum은 구조체 안에서 `int32_t`로 저장
* 고정폭 정수 타입만 사용
* DLL 경계를 넘는 구조체 배치나 내보낸 함수 시그니처가 바뀌면 ABI 버전 증가

`PWR_Frame` 안의 포인터는 `pwr_engine_frame_acquire()` 성공과 짝이 되는
`pwr_engine_frame_release()` 사이에서만 유효합니다.

---

## 9. 스레드 규약

* `pwr_engine_create` / `_destroy` — 호출자가 직렬화
* `pwr_engine_frame_acquire` / `_release` — 소비자 스레드 1개에서 워커와 동시 안전
* 그 외 모든 `pwr_engine_*` 세터 — 내부 락, 어느 스레드에서든 안전

락은 두 개뿐이고 절대 중첩되지 않으므로 순서에 의한 교착이 불가능합니다:
`ctrl_lock`은 실행 상태와 조건변수를, `proc_lock`은 1 CPI 처리 전체 구간과
모든 변경자를 감쌉니다. 워커는 `ctrl → 해제 → proc`, 변경자는
`proc → 해제 → ctrl` 순서로만 잡습니다.

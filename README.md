# PWRadarSystem

해상·공중 감시용 PW(Pulsed Waveform) 레이다 탐지 시스템.
순수 C17, 외부 라이브러리 의존성 없음, Windows / Linux 크로스플랫폼.

| 프로젝트 | 산출물 | 역할 |
|---|---|---|
| `PWRadarCore` | `PWRadarCore.dll` / `libPWRadarCore.so` | 신호처리 엔진 (시뮬레이터 → 펄스압축 → MTI → Doppler → CFAR → 클러스터링 → 트래커) |
| `PWRadarUI`   | `PWRadarUI.exe` / `PWRadarUI`           | 실시간 탐지 검증 콘솔. MATLAB 스타일 플로팅 자체 구현 |

링크 대상은 OS가 기본 제공하는 것뿐. Windows는 `user32` + `gdi32`,
Linux는 `libX11` + `pthread` + `libm`. 수치 라이브러리·GUI 툴킷·폰트
라이브러리 미사용. FFT, 난수 생성기, 칼만 필터, 안티에일리어싱 렌더러,
폰트 래스터라이저 모두 저장소 내 구현.

---

## 1. 빌드

CMake가 빌드 기술의 단일 소스. Windows용 `.sln` / `.vcxproj`는 저장소에
포함하지 않고 CMake의 Visual Studio 제너레이터가 생성. 생성된 솔루션은
MSBuild 자신의 산출물이므로 IDE 로딩이 보장됨.

### Windows

```bat
build.bat                :: Release x64  → 구성 + 빌드 + 수치 검증
build.bat Debug          :: Debug
build.bat Release run    :: 빌드 후 콘솔 실행
build.bat clean          :: build 디렉터리 삭제
```

빌드 후 IDE 작업은 `build\PWRadarSystem.sln` 열기. F5로 바로 실행됨.

`build.bat`은 CMake를 PATH → Visual Studio 2022 번들 → 독립 설치 순으로
탐색하므로 별도 설정 불필요. 요구 조건은 Visual Studio 2022 +
**"C++를 사용한 데스크톱 개발"** 워크로드.

산출물: `build\Release\PWRadarUI.exe`, `build\Release\PWRadarCore.dll`

### Linux

```sh
./build.sh                # Release → 구성 + 빌드 + 수치 검증
./build.sh Debug
./build.sh Release run
./build.sh clean
```

산출물: `build/PWRadarUI`, `build/libPWRadarCore.so`
사전 설치: `sudo apt install build-essential cmake libx11-dev`

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

CMake 없이 Linux에서 빌드하려면 `make -j && make test`.

경고 정책: GCC/Clang은 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`
계열, MSVC는 `/W4`에서 무경고 빌드. 경고를 오류로 승격하는
`-DPWR_WERROR=ON`은 옵트인. 컴파일러 신규 진단 때문에 첫 빌드가 실패하는
것을 막기 위한 기본값.

### Windows 코드 경로 교차 검증

Linux에서 MSVC 없이 Win32 계층 전체를 컴파일·링크·실행 가능.

```sh
sudo apt install mingw-w64
./tools/check_windows_build.sh
```

세 단계 게이트로 구성.

1. **이름 충돌 게이트** — `tools/check_name_collisions.py`가 공개 헤더의 모든
   식별자를 Windows SDK 헤더의 오브젝트형 매크로 전체와 대조. §1.1 참조.
2. **컴파일·링크** — `-Werror`로 `PWRadarCore.dll`(`pwr_*` 심볼 60개 export) +
   `PWRadarUI.exe` 생성. 창 프로시저, DIB 프레임버퍼, Win32 스레딩/원자연산,
   dllexport/dllimport 배선 전 구간 통과.
3. **실행** — wine 설치 시 `PWRadarUI.exe --selftest` 실제 실행.

### 1.1 상태 코드 명명 규약

성공 코드는 `PWR_OK`가 아니라 `PWR_STATUS_OK`. `<winuser.h>`가 레거시
`WM_POWER` 브로드캐스트 상수를 오브젝트형 매크로로 정의하기 때문.

```c
#define PWR_OK              1        /* winuser.h */
#define PWR_FAIL            (-1)
#define PWR_SUSPENDREQUEST  1
#define PWR_CRITICALRESUME  3
```

매크로는 스코프와 무관하게 열거자를 이김. `<windows.h>`에 닿는 번역 단위에서
`PWR_OK = 0` 열거자가 전처리기에 의해 `1`로 치환되면 `return PWR_OK;`가
`return 1;`이 되고 호출자의 `status != PWR_STATUS_OK` 검사가 성사됨.
`-Werror`를 무경고 통과하며 Linux 빌드는 영향 없음.

방어는 세 겹.

| 방어선 | 위치 | 성질 |
|---|---|---|
| 이름 미사용 | `PWR_STATUS_OK`로 명명 | 근본 해결 |
| 컴파일 타임 가드 | `PWRadarCore/src/pwr_guard.c` | `<windows.h>`를 **먼저** 인클루드하는 전용 TU에서 `#if defined(...)` + `_Static_assert` |
| 빌드 게이트 | `tools/check_name_collisions.py` | 인클루드 순서와 무관하게 SDK 전체 스윕 |

`#undef PWR_OK`는 해결책이 아님. 해당 매크로를 정당하게 사용하는 소비자가
있고, 인클루드 순서가 바뀌면 충돌이 재발.

`pwr_status.h` 안의 `#if defined(PWR_OK)` 트립와이어는 동작하지 않음. 실제
번역 단위는 공개 헤더를 `<windows.h>`보다 먼저 인클루드하므로 트립와이어
평가 시점에 해당 매크로가 아직 존재하지 않음. 그래서 가드가 인클루드 순서를
통제할 수 있는 `pwr_guard.c`에 위치.

### 명령행 옵션

```
--selftest             코어 수치 검증 스위트 실행 후 종료 (CI 게이트용, 실패 시 non-zero)
--scenario N           시작 시 N번 시나리오 로드
--list-scenarios       시나리오 목록 출력
--capture N FILE       N 프레임 렌더 후 프레임버퍼를 PPM으로 저장하고 종료 (헤드리스 회귀 검증)
--version / --help
```

---

## 2. 설계

### 2.1 모든 dB 값은 SNR로 캘리브레이션

시뮬레이터가 복소 샘플당 열잡음 분산을 정확히 1.0으로 주입하고, 신호처리
체인이 그 기준을 끝까지 보존.

> Range-Doppler 맵, A-scope, RTI 워터폴의 dB 값은 곧 SNR. 잡음 바닥이 0 dB.

화면에서 18 dB로 읽히는 표적은 적분 후 SNR 18 dB. 따라서 임계값·탐지·트랙
품질 수치에 추가 스케일링이 불필요하고, 디스플레이를 SNR 계측기로 그대로
사용 가능.

정규화 상수는 가정이 아니라 측정값(`pwr_waveform.c`). 압축 필터를 단위 피크
이득으로 정규화한 뒤, 단위 분산 백색 입력에 대한 실제 진폭 이득
(`noise_gain`)을 파스발 관계로 계산해 `sigma_pc`로 사용.

### 2.2 펄스압축은 시간영역 테이퍼가 아니라 스펙트럼 등화

```
H(f) = conj(TX(f)) · W(f) / ( |TX(f)|² + ε )
```

LFM 레플리카에 시간영역 창을 곱하는 구현은 시간↔주파수 정상위상 근사에
의존하며, 오차는 `1/sqrt(Tp·B)`로 감소. 실제 감시 레이다의 시간대역폭적
100 수준에서는 LFM 스펙트럼의 Fresnel 리플이 ±Tp 전 구간에 −40 dBc 수준의
평탄한 페어드 에코 대(pedestal)를 남김. 대형 선박 양쪽 ±2.9 km에 유령 탐지를
만들 정도의 크기. `|TX(f)|`를 등화하면 선택한 테이퍼의 설계 부엽 레벨이
실제로 구현됨. `ε`은 등화 동적범위와 잡음 증폭의 상한이며, 그로 인한
정합손실은 측정·보고됨.

측정 결과 (정합손실 전부 0.05 dB 이하):

| 거리 테이퍼 | 달성 PSL | 주엽 폭 | 정합손실 |
|---|---|---|---|
| Rectangular | −25.4 dB | 2.0 bin | 0.42 dB |
| Taylor −35 dB | **−37.4 dB** | 2.0 bin | 0.05 dB |
| Taylor −50 dB | −47.9 dB | 2.0 bin | 0.02 dB |
| Hamming | −43.7 dB | 2.0 bin | 0.02 dB |
| Blackman | −64.5 dB | 4.0 bin | 0.01 dB |
| Chebyshev −60 dB | −64.4 dB | 4.0 bin | 0.01 dB |

### 2.3 Pfa는 셀 개수가 아니라 셀 발생률 기준

기본 형상은 CPI당 1000 × 64 셀을 검사하고 1회전당 약 234 CPI를 처리하므로
회전당 1.5e7 셀. "스캔당 오경보 1개"를 목표로 하면 Pfa ≈ 1e-7. 관습적으로
쓰이는 1e-6은 스캔당 오경보 15개를 발생시켜 트랙 파일을 tentative 트랙으로
채움. 기본값은 **1e-7**이며, 제어 패널이 셀당 Pfa와 스캔당 설계 플롯 수를
함께 표시.

### 2.4 회전 안테나 트랙 관리

M-of-N 히트/미스는 스캔(안테나 1회전)당 1회 적립. 드웰 병합된 플롯(§2.7)은
빔이 표적을 지나간 뒤 도착하므로 조사 CPI마다 판정 불가. 대신 조준선이 트랙
방위를 LAG(빔폭의 1.6배 + 2 CPI 여유)만큼 지나치는 순간, 즉 그 드웰의 플롯이
이미 발행·연관된 시점에 1회 판정.

- 예측이 뒤처져 교차가 플롯보다 먼저 와 미스가 기록된 경우, 이후 연관이
  장부를 소급 정정.
- 역행 겉보기 운동으로 교차 창을 건너뛴 트랙은 시간 기반 캐치업(1.25 스캔)이
  회전당 1회 판정을 보장.
- 연관 후보는 고정 방위 창이 아니라 비용 행렬과 동일한 직사각 미터 게이트
  (`gate_max_range_m`)로 선정. 방위 창은 통계 게이트보다 좁아, 접선 150 m/s
  횡단 표적처럼 스캔당 10°를 이동해도 예측에서 375 m밖에 떨어지지 않은
  플롯을 자기 트랙에서 분리시킴.
- 스캔 기반 노후화 백스톱 유지. 커버리지 밖으로 나가 빔이 더 이상 지나지 않는
  트랙 회수용이며, 시한은 `delete_misses + 2` 스캔이므로 운용자의 삭제 설정을
  선점하지 않음.
- 정지응시(스캔 0 rpm)에서는 CPI당 1회 판정.

도플러 게이트는 측정이 ±V_ua로 접혀 들어오는 것을 감안해 **2·V_ua 모듈로**로
비교. 접힘을 무시하면 실제 시선속도가 모호 경계를 넘는 순간(예: 횡단 표적의
방위가 커질 때) 수렴한 트랙이 모든 후속 플롯을 기각하고 소멸. 시나리오 6의
검증 항목.

### 2.5 분산 클러터는 압축 영역에 주입

해면/체적 클러터는 송신 펄스 에코의 조밀한 중첩. 원시 I/Q에 변조되지 않은
난수열을 더하면 두 가지가 틀림. 정합필터가 이를 압축하지 않고 펄스 길이
전체로 확산시키며, 레벨도 압축이득만큼 낮게 나옴.

반사율 필드를 처프와 컨볼루션하면 정확하지만 펄스당 고속시간 FFT 2회가 추가.
압축이 선형이고 표면 반사율이 백색이므로, 압축 후 클러터의 통계는 압축
데이터에 직접 더한 백색 필드와 (압축 펄스폭 상관거리 이내에서) 동일. 따라서
펄스압축 직후·MTI 직전에 주입.

펄스 간 상관은 AR(1) 재귀로 구현하며, 계수가 설정된 폭의 가우시안 클러터
스펙트럼을 재현하므로 MTI와 도플러 필터뱅크가 올바른 내부 클러터 운동을
관측함.

### 2.6 프레임 발행

3중 버퍼. 생산자는 항상 "최신 발행 슬롯도 아니고 소비자가 점유한 슬롯도
아닌" 슬롯에 기록. 3개면 그런 슬롯이 항상 존재하므로 생산자는 블록되지 않고
소비자는 찢어진 프레임을 보지 않음.

### 2.7 플롯은 CPI가 아니라 드웰 단위

CPI별 탐지의 방위각은 그 CPI의 빔 조준선이고, 강한 표적은 빔이 지나가는 여러
연속 CPI에서 모두 임계값을 초과(SNR 66 dB 표적은 2-way −50 dB 지점까지 탐지).
병합 없이 통과시키면 표적 하나가 스캔마다 방위 수 도(°)에 걸친 플롯 문자열을
생성하며, 두 가지 문제가 발생.

1. 남는 가장자리 플롯이 개시 억제 반경을 벗어나 **중복 트랙**을 생성.
2. CPI당 1.5°씩 스텝하는 방위 갱신이 칼만 필터에 빔 회전 방향의 **가짜 접선
   속도**를 주입해 실속도와 무관한, 때로는 반대 방향의 벡터를 산출.

따라서 실장비 플롯 추출기와 동일하게 드웰 단위로 중심추정. 연속 CPI에서
거리·시선속도가 한 셀 이내로 일치하는 히트를 하나의 드웰 플롯에 전력 가중
누적하고, 빔이 지나가면(기여 없는 CPI 1회) 닫아서 발행. 방위는 2-way 빔
형상에 대한 전력 가중 중심, 즉 고전적 빔 분할(beam splitting) 추정기이므로
빔폭의 수분의 일 정확도를 확보.

거리/속도 게이트는 의도적으로 한 셀 수준으로 좁게 설정. 분해능 페어(시나리오
4)가 병합되지 않게 하기 위함이며, 같은 CPI의 플롯끼리는 정의상 서로 다른
표적이므로 병합 대상에서 제외. 정지응시에서는 드웰이 종료되지 않으므로 이
단계를 우회.

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

모든 경계는 드래그 가능한 스플리터. 모든 디스플레이는 독립적인 줌·팬·데이터
커서를 보유.

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

틱은 1-2-5 알고리즘으로 자동 생성. dB축 가독성을 위해 2.5 배수 포함. 트레이스는
목적지 컬럼당 min/max 데시메이션을 수행하므로 창이 좁아도 부엽 스파이크가
소실되지 않음. 오실로스코프와 동일한 방식.

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
| `T` `G` `H` | 실제값 오버레이 / 공분산 타원 / 히스토리 오버레이 |
| `1`–`6` `F1` | 제어 패널 탭 / 도움말 |

### 히스토리 오버레이와 Verify 탭

드웰 → 히트 → 플롯 → 트랙으로 이어지는 처리 사슬을 화면에서 직접 검증 가능.
PPI에 다음 네 계층이 중첩됨.

1. **플롯 히스토리** — 보존 시간 동안 유지. 청록은 트랙에 연관된 플롯, 황색은
   미연관 플롯이므로 오경보 산포가 그대로 드러남.
2. **전체 경로 폴리라인** — 트랙·실제값 경로. 나이에 따라 감쇠하며, 확정 트랙
   소실 지점은 ×로 표시.
3. **연관선** — 이번 드웰에 어떤 플롯이 어떤 트랙을 갱신했는지 연결.
4. **드웰 히트/미스 링** — 트랙 심볼 위. 초록은 히트, 빨강 점선은 미스.

트랙 테이블의 HIT 열과 선택 트랙 판독의 dwell window는 M-of-N 확인 논리가
집계하는 비트를 그대로 표시. 우측 **Verify 탭**은 실제값-트랙 자동 페어링으로
표적별 추적 상태·현재 오차·RMSE·추적 완성도(CMP%)·최초 트랙 소요시간(TTT)과
전역 요약(추적률·가짜 트랙·중복 트랙)을 집계. 보존 시간과 레이어는 View 탭에서
조절.

트랙 속도는 화살촉이 달린 이동 방향 벡터(MIL-STD-2525 스타일)로 표시. 길이는
View 탭의 leader 시간(기본 60 s) 동안의 이동 거리이고, 30초 이동량마다 눈금이
찍히므로 거리 링과 대조해 속도를 정량적으로 판독 가능. `T` 오버레이를 켜면
실제 침로가 점선 화살표로 중첩되어 추정 침로 오차가 두 화살촉 사이의 각도로
표시됨.

---

## 4. 검증 시나리오

`--list-scenarios` 또는 툴바 드롭다운으로 선택.

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

`PWRadarUI --selftest`로 실행. CTest에도 `core_selftest`로 등록됨.

출력 예 (GCC 13.3 / Linux x86-64 Release):

```
T1 FFT vs direct DFT            fwd rel 1.10e-07, round trip 1.26e-07
T2 amplitude tapers             9 tapers checked
T3 sort and quickselect         97 samples
T4 global nearest neighbour     optimum 6, got 6.0
T5 tracker linear algebra       2x2 inverse, gate, symmetrise
T6 pulse compression range      truth 12000 m, peak 11992 m
T7 Doppler axis calibration     truth -35.0 m/s, peak -34.6 m/s
T8 CFAR false-alarm rate        design 1.0e-04, achieved 8.63e-05 over 614400 cells
T9 end-to-end tracking          1 confirmed, best |dy| = 293 m at t = 0.42 s
9/9 cases passed
```

각 항목은 통과 여부만 고정이고 수치는 컴파일러·최적화에 따라 변동. 판정
기준은 항목별 허용범위이며, 예를 들어 T8은 설계 Pfa의 0.05배–20배 구간.
유한 표본 추정이고 클러스터링이 인접 경보를 병합하므로 한 자릿수 이내 일치가
유효 범위.

T4는 greedy 알고리즘이 8을 선택하는 3×4 문제를 사용. 증강경로 탐색의 실제
동작 여부를 확인하기 위함.

---

## 6. 기본 형상

S밴드 연안 감시 레이다.

| 항목 | 값 |
|---|---|
| 반송파 / 대역폭 / 펄스폭 | 3.05 GHz / 5 MHz / 20 µs (TB = 100, 압축이득 20 dB) |
| 표본율 / PRF | 6.25 MHz / 3000 Hz |
| 거리 분해능 / 셀 간격 | 30.0 m / 24.0 m |
| 비모호 거리 / 속도 | 50.0 km / ±73.7 m/s |
| 속도 분해능 | 4.61 m/s |
| CPI / 스캔 주기 | 32 펄스, 10.67 ms / 2.5 s (24 rpm, 방위 빔폭 1.6°) |
| 처리 격자 | 1000 거리 빈 × 64 Doppler 빈 |
| 안테나 이득 | 29 dBi (1.6°×20° 개구의 실제 이득. 링크 예산 자체 일관성 확보) |
| 잡음 전력 | −103.0 dBm |
| 탐지거리 (13 dB SNR) | 0.1 m² 18.7 km / 1 m² 33.2 km / 100 m² 105 km |

도플러 FFT는 32 펄스를 64 빈으로 2배 오버샘플. 따라서 속도 분해능(4.61 m/s)과
축 간격(2.30 m/s)이 다름.

단계별 처리 시간은 상태바와 DSP 탭에 EWMA로 표시되며, 부하율은
`t_total / CPI 예산`. 부하율이 1을 넘는 환경에서는 `speed` 슬라이더(0.05×–8×)로
슬로모션 적용 가능. 시나리오 시간은 벽시계와 분리되어 있으므로 물리적 일관성이
유지됨.

---

## 7. 소스 구성

```
PWRadarSystem/
├── CMakeLists.txt                   빌드 기술의 단일 소스 (Windows/Linux 공통)
├── build.bat                        Windows: .sln 생성 + 빌드 + 수치 검증
├── build.sh                         Linux:   구성 + 빌드 + 수치 검증
├── Makefile                         CMake 없이 쓰는 Linux 대안
├── PWRadarCore/
│   ├── include/pwradar/             공개 C ABI (DLL 경계를 넘는 유일한 헤더 집합)
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
│       │                            <windows.h>를 먼저 인클루드해 이름 충돌을
│       │                            검출하고, 상태코드 ABI 값과 PWR_Complex
│       │                            레이아웃을 _Static_assert로 고정. §1.1 참조
│       └── pwr_core.h  pwr_names.c
├── PWRadarUI/src/
│   ├── ui_platform.h                OS 추상화 (창 1개 + 입력 + 블릿이 전부)
│   ├── ui_platform_win32.c          user32 + gdi32 (DIB 섹션 → BitBlt)
│   ├── ui_platform_x11.c            libX11 (XImage → XPutImage)
│   ├── ui_gfx.[ch]                  소프트웨어 렌더러 (AA 라인·폴리곤·원·텍스트·필드 블릿)
│   ├── ui_font.h / ui_font_data.c   임베디드 AA 글리프 아틀라스 (생성물, 57 KB)
│   ├── ui_colormap.[ch]             컬러맵 10종
│   ├── ui_theme.h                   색·치수 단일 정의처
│   ├── ui_widget.[ch]               컨트롤 세트 + uitable + 스플리터
│   ├── ui_plot.[ch]                 MATLAB 대응 axes / 시리즈 / imagesc / colorbar
│   ├── ui_ppi.[ch]                  PPI 스코프 (극좌표 LUT 캐시, 심볼로지)
│   ├── ui_history.[ch]              표시용 경로·플롯 히스토리 누적
│   ├── ui_app.[ch]                  레이아웃·제어패널·4개 디스플레이·코어 바인딩
│   └── main.c
└── tools/
    ├── gen_font.py                  폰트 아틀라스 생성기 (빌드에 관여하지 않음)
    ├── check_name_collisions.py     공개 식별자 vs Windows SDK 매크로 스윕 (§1.1)
    └── check_windows_build.sh       mingw-w64 교차 검증 + wine 실행 게이트
```

OS 접점은 `ui_platform.h` 하나뿐이고 그 위의 모든 픽셀은 `ui_gfx.c`의
소프트웨어 렌더러가 생성. 따라서 Windows와 Linux가 픽셀 단위로 동일한 화면을
출력. 픽셀 포맷 0xAARRGGBB는 Win32 BI_RGB DIB와 X11 24/32비트 TrueColor 양쪽에
무변환 대응.

폰트 아틀라스는 오프라인 생성 후 커밋된 산출물. 콘솔이 C 런타임 외에 아무것도
링크하지 않으면서 안티에일리어싱된 텍스트를 렌더링하기 위함. 페이스·크기·글리프
집합 변경 시에만 `python3 tools/gen_font.py PWRadarUI/src/ui_font_data.c` 재실행.

---

## 8. ABI 규약

`pwr_types.h`의 구조체 배치는 `PWR_ABI_VERSION`에 종속. `PWRadarUI`는 시작 시
`pwr_abi_version()`을 자신이 컴파일된 값과 비교하고 불일치 시 실행을 거부.

유지보수 규칙.

- 멤버는 정렬이 큰 것부터 배치. MSVC x64와 GCC/Clang x86-64에서 배치가 동일해야 함
- 비트필드 금지, `bool` 금지, enum은 구조체 안에서 `int32_t`로 저장
- 고정폭 정수 타입만 사용
- DLL 경계를 넘는 구조체 배치나 내보낸 함수 시그니처가 바뀌면 ABI 버전 증가
- 상태 코드 숫자 값은 ABI의 일부. 재번호 부여 금지, 추가만 허용

`PWR_Frame` 안의 포인터는 `pwr_engine_frame_acquire()` 성공과 짝이 되는
`pwr_engine_frame_release()` 사이에서만 유효.

---

## 9. 스레드 규약

| 함수군 | 규약 |
|---|---|
| `pwr_engine_create` / `_destroy` | 호출자가 직렬화 |
| `pwr_engine_frame_acquire` / `_release` | 소비자 스레드 1개에서 워커와 동시 안전 |
| 그 외 `pwr_engine_*` 세터 | 어느 스레드에서든 안전하고 **논블로킹** |
| `pwr_engine_reconfigure`, 표적 목록 변경자 | 현재 CPI 종료까지 대기 (최대 1 CPI + 재구성 수 ms) |

논블로킹 세터의 변경은 펜딩 메일박스에 적재되어 워커가 다음 CPI 시작 시 반영.
엔진이 유휴 상태(정지·일시정지·수동 스텝)면 즉시 적용. 호출 스레드는 어떤
경우에도 진행 중인 CPI 종료를 기다리지 않음. 워커가 부하율 1을 넘겨 `proc_lock`을
상시 점유하는 상황에서도 UI 슬라이더 드래그가 CPI 시간만큼 멈추지 않게 하기 위한
설계.

블로킹 변경자의 1 CPI 상한은 공정성 게이트가 보장. 일반 뮤텍스는 대기자에게
소유권을 넘기지 않으므로 포화된 워커가 락을 즉시 재획득하며 뮤테이터를 수 초씩
지연시킬 수 있음. 뮤테이터가 대기를 선언하면 워커가 CPI 사이에서 양보.

락 순서에 의한 교착은 불가능. `ctrl_lock`은 실행 상태와 조건변수를, `proc_lock`은
1 CPI 처리 전체 구간과 재구성을 감싸고, `pending_lock`은 메일박스 복사 구간에만
잡는 리프 락. 워커는 `ctrl → 해제 → proc → pending`, 변경자는 `proc → 해제 → ctrl`
또는 `pending` 단독으로만 획득하므로 중첩 방향이 `proc → pending` 하나뿐.

버퍼 크기가 바뀌는 재구성에서도 트랙 파일은 보존됨. 트래커 상태가 격자 인덱스가
아니라 ENU 미터 좌표이므로 격자 변경으로 무효화되지 않음. 새 커버리지 밖에 남은
트랙은 플롯을 더 받지 못해 스캔 노후화 규칙으로 소거됨.

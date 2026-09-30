# algotradelib — 한국어 요약

<!-- multilingual: start -->
[![en](https://img.shields.io/badge/lang-en-red.svg?style=plastic)](README.md)
[![ko-kr](https://img.shields.io/badge/lang-ko--kr-green.svg?style=plastic)](README.ko-kr.md)
<!-- multilingual: end -->

<!-- badges: start -->
![License](https://img.shields.io/badge/license-MIT-blue.svg?style=plastic)
[![Lifecycle: experimental](https://img.shields.io/badge/lifecycle-experimental-orange.svg?style=plastic)](https://lifecycle.r-lib.org/articles/stages.html#experimental)
<!-- badges: end -->

<!--
  동기화 기준: 영어 README.md의 c5d938e 시점 내용 (2026-09-30).
  각 절 위의 "en:" 주석은 그 절이 옮긴 영어 절입니다. `git diff c5d938e HEAD -- README.md`에서 그 절이
  바뀌었다면 이 페이지도 고치거나, 그 브랜치의 PR 문서에 한국어 페이지가 뒤처졌다고 적으세요.
-->

> **이 문서는 요약이며, 기준 문서는 [영어 README](README.md)입니다.** 두 문서가 다르면 영어 README가
> 맞습니다. 이 페이지는 2026-09-30, 커밋 `c5d938e` 시점의 영어 README 내용에 맞춰 작성되었습니다.
> 여기에 없는 내용은 영어 README와 [사용자 가이드](doxygen/vignettes/guide.md)(영어)를 보세요.

<!-- en: the introduction @ c5d938e -->
한국 증권사 API(한국투자증권(KIS), 키움증권, 토스증권)로 이벤트 기반 자동매매를 만들기 위한 C++17
헤더 전용 라이브러리입니다. CMake로 가져다 씁니다. 전략은 I/O를 하지 않고 증권사를 모르는
평범한 구조체이므로, 같은 소스를 과거 데이터로 재생(백테스트)하고, 인증 정보 없이 모의 실행(dry
run)한 뒤, 고쳐 쓰지 않고 실거래에 돌릴 수 있습니다. 증권사 선택은 타입 매개변수 하나입니다. 단,
키움에서 러너를 돌리려면 아래 "먼저 읽어 주세요"의 몇 가지를 먼저 명시해야 합니다.

## 먼저 읽어 주세요: 실제 돈이 움직입니다

<!-- en: the opening warning, "Brokers and accounts", and the Kiwoom sentences in "Using it" @ c5d938e -->

- **이 라이브러리는 실제 계좌로 실제 주문을 보냅니다. 그런데 아직 증권사 계좌를 상대로 처음부터 끝까지
  실행해 본 적이 없습니다** (2026-09-30 기준). 라이브러리가 기대는 증권사 동작은 모두 증권사 명세서를
  읽고 구현한 것입니다. 개발 중인 버전(`0.0.0.9000`)이며 아직 릴리스가 없습니다. 먼저 모의투자
  계좌로 연습하세요.
- **KIS와 키움은 실전투자라고 적지 않는 한 모의투자입니다.** 실제 돈이 움직이는 실전투자만은 반드시
  명시해야 합니다: 환경 변수 `KIS_DEMO=0` / `KIWOOM_DEMO=0`, 인증 정보 파일의 `"demo": false`, 또는
  코드의 `is_demo_ = false`. 아무것도 적지 않으면 증권사의 모의투자 서버를 쓰고, 모의투자 서버는
  실전 키를 거부하므로 설정을 잊으면 거래가 되는 대신 실패합니다. 값을 설정했다면 정확히 `1` 또는
  `0`이어야 하며, 그 밖의 값은 설정 읽기 자체가 거부됩니다.
- **토스증권에는 모의투자 환경이 없습니다.** 인증 정보를 넣은 토스 프로그램은 실제 돈으로
  거래합니다. 전략은 먼저 재생이나 모의 실행으로, 또는 KIS·키움 모의투자 계좌에서 연습하세요.
- **키움의 잔고 조회에는 사용자의 명시가 필요합니다.** 잔고를 가져오는 `kt00018`(계좌평가잔고내역)은
  거래소 구분을 받지만, 그것이 종목을 거래소별로 걸러 내는지 평가 가격만 정하는지는 명세에 없습니다.
  추측하지 않기 위해, `KiwoomConfig::holdings_venue_`와 `holdings_position_scope_`를
  (매도 가능 수량 검사를 거치는 매도에는 `holdings_sellable_scope_`도) 설정하기 전까지 어댑터는 잔고 조회를 거부하며, 따라서 잔고에 기대는
  매도 가능 수량 확인과 포지션 확인도 모두 거부됩니다. 각 선택은 멤버 문서에 설명되어 있습니다.
- **키움 전략 러너에는 주문 경로를 명시해야 합니다.** 키움은 모든 주문에 거래소를 요구하는데, 전략이
  만드는 주문에는 거래소가 없습니다. 러너를 기본 경로와 함께 바인딩하세요:
  `live::bind_journaled(session, journal, strategy, limits, live::default_series_capacity, Venue::KRX)`
  (또는 `Venue::NXT`). 키움 SOR인 `Venue::COMBINED`는 아직 쓰지 마세요: SOR 주문이 주문 조회에서
  어떻게 보이는지 관찰된 적이 없고, 장애 후 복구가 그것에 달려 있습니다(`SessionExecutor::default_route`
  문서 참고). 경로가 없으면 어댑터는 주문을 보내기 전에 거부합니다. `example-live-program`은 KRX를
  명시합니다.
- **키움은 거래 달력을 제공하지 않습니다.** 그래서 러너는 장 운영 단계를 알 수 없고, 기본 한도로는
  신규 포지션을 열지 않습니다. 기본 상태 그대로의 `example-live-program`은 키움에서 관찰만 하고
  **주문을 내지 않습니다**: 잔고 조회의 범위를 명시하고 장 운영 단계 제한을 풀어야 주문합니다.
- **KIS는 본인 계좌 고객만 지원합니다.** 라이브러리는 `custtype P`(일반)를 보내며, 이는 개인과
  법인 고객이 자기 계좌로 거래하는 경우를 모두 포함합니다. 제3자를 대리하는 제휴사용 방식
  (`custtype B`)은 지원하지 않습니다. KIS 요청 명세에는 `B : 법인`이라고 적혀 있지만, KIS 예제 코드는
  `일반(개인고객,법인고객) "P", 제휴사 "B"`라고 설명하며 라이브러리는 이쪽을 따릅니다.
- **KRX와 NXT를 아우르는 주문 라우팅(SOR)은 선택 사항입니다.** KIS에서 거래소를 지정하지 않은 주문은
  KRX로 갑니다. KIS에서는 `Venue::COMBINED`가 SOR을 요청하며, 러너는 `Venue::COMBINED`를 기본 경로로
  바인딩해야 SOR을 씁니다. KIS 모의투자 계좌가 SOR을 거부하기 때문에 기본값이 아닙니다: 모의투자에서
  실패하는 기본값으로는 실거래를 미리 연습할 수 없습니다. 키움과 마찬가지로, SOR이 관찰되기 전까지는
  KRX나 NXT를 쓰세요: KIS가 주문 조회에서 SOR 주문을 SOR로 표시하는지 주문이 걸린 거래소로
  표시하는지 알 수 없고, 장애 후 복구가 그것에 달려 있습니다. 키움 주문은 반드시 거래소를 지정해야
  하고, 토스는 선택지가 없습니다.

## 지원 증권사

<!-- en: the table in "Brokers and accounts" @ c5d938e -->

| 증권사 | 상태 | 모의투자 |
| --- | --- | --- |
| KIS | 시세, 거래 달력, 종목 정보, 잔고, 매수 가능 금액, 주문(스톱지정가 포함), 주문 내역 | 기본값 (실전은 `KIS_DEMO=0`) |
| 키움 | 시세, 종목 정보, 매수 가능 금액, 세션을 통한 주문(스톱지정가 포함)과 주문 내역; **거래 달력 없음**; **잔고는 명시 전까지 거부**; **전략 러너는 경로 명시 필요** (위 참고) | 기본값 (실전은 `KIWOOM_DEMO=0`) |
| 토스 | 시세, 거래 달력, 종목 정보, 잔고, 매수 가능 금액, 주문(스톱지정가 없음), 주문 내역 | **없음** |

## 빠른 시작

<!-- en: "Prerequisites" (OpenSSL), "Install", "Build options", "Using it", "Documentation" @ c5d938e -->

필요한 것: C++17 컴파일러, CMake, Boost(`json`), OpenSSL, 그리고 분석 기능용 Eigen. 최소 버전은 영어
README의 "Prerequisites"에 있습니다. **OpenSSL은 3.0 이상이면 빌드되지만, 3.0의 공식 지원은 2026년
9월에 끝났습니다.** 거래에 쓰는 환경에는 지원 중인 버전(3.5 LTS 이상)이나 보안 수정을 계속 받는
배포판 패키지를 쓰세요. 이 라이브러리로 빌드하고 시험한 버전은 3.6.x뿐입니다.

새 프로젝트 만들기 (자세한 내용은 영어 README의 "Install"):

```sh
tools/atl-new-project.sh my-alpha
```

서브모듈로 가져오기:

```sh
git submodule add https://github.com/ygeunkim/algotradelib.git libs/algotradelib
git submodule update --init --recursive
```

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_alpha LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_subdirectory(libs/algotradelib)

add_executable(my_alpha src/main.cpp)
target_link_libraries(my_alpha PRIVATE algotradelib::algotradelib)
```

라이브러리 자체 검사와 HTML 문서 빌드:

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON -DATL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build/tests --output-on-failure
cmake --build build --target docs      # Doxygen이 있을 때, doxygen/docs/html/index.html
```

실거래 프로그램은 영어 README의 "Using it"을 보세요. 모의투자로 먼저 실행합니다:

```sh
export KIS_APP_KEY=... KIS_APP_SECRET=... KIS_CANO=... KIS_DEMO=1
./build/bin/example-live-program kis 005930           # 관찰만: 실제 데이터, 아무것도 보낼 수 없음
./build/bin/example-live-program kis 005930 --live    # 저널을 통해 거래
```

## 용어 대조표

라이브러리의 이름과 증권사 문서의 용어를 잇는 표입니다. 필드 이름과 한글 이름은 각 증권사 API
명세의 것입니다. **주문 코드 번호는 증권사마다 뜻이 다릅니다**(예: 키움 `28`은 스톱지정가지만 KIS
`28`은 다른 주문입니다). 코드는 항상 증권사 이름과 함께 읽으세요.

| 라이브러리 | 증권사 용어 | 비고 |
| --- | --- | --- |
| 데모(demo) / 라이브(live) | 모의투자 / 실전투자 | KIS·키움은 기본이 모의투자, 토스는 모의투자 없음 |
| `Venue::KRX` | 한국거래소(KRX) | |
| `Venue::NXT` | 넥스트레이드(NXT) | 키움 명세에는 "넥스트트레이드"로 표기됨 |
| `Venue::COMBINED` | SOR (주문 라우팅) | 주문 경로로서의 SOR. 통합 시세나 조회 화면의 "통합"과는 다른 개념. KIS·키움 모두 관찰 전까지 비권장 |
| `Venue::UNSPECIFIED` | (주문 경로) 미지정 | 주문에서: KIS는 KRX로 보내고, 키움은 거부. 키움 잔고용 `holdings_venue_`에서는 `UNSPECIFIED`와 `COMBINED` 모두 거부 |
| `OrderSession::REGULAR` | 정규장 | |
| `OrderSession::OPENING_AUCTION` | 시가 단일가 | 토스 `OPG` |
| `OrderSession::PRE_MARKET` | 장전 시간외 | 전일 종가로 거래 |
| `OrderSession::AFTER_HOURS` | 장후 시간외 | 당일 종가로 거래 |
| `OrderSession::AFTER_HOURS_AUCTION` | 시간외 단일가 | |
| `OrderType::STOP_LIMIT` | 스톱지정가 | KIS 주문구분 `22`, 키움 매매구분 `28`; 토스는 없음 |
| `HoldingPosition::sellable_quantity_` | 매도 가능 수량 | KIS는 잔고 행의 `ord_psbl_qty`(주문가능수량)를 씀 — 별도의 매도가능수량조회 API는 쓰지 않음. 키움은 `kt00018`의 `trde_able_qty`(매매가능수량). 미체결 매도 주문을 이미 뺀 수량인지는 두 증권사 모두 밝히지 않음 |
| `BuyingPower::cash_` | 신용·미수 없이 살 수 있는 금액 | KIS `nrcvb_buy_amt`(미수없는매수금액; `max_buy_amt` 최대매수금액이 아님), 키움 `kt00001`의 `100stk_ord_alow_amt`(100%종목주문가능금액; `ord_alow_amt` 주문가능금액이 아님), 토스 `cashBuyingPower`. 증권사 앱의 "주문가능금액"이나 "최대매수금액"보다 작을 수 있음. 그 차이는 미수·신용·대용금처럼 현금이 아닌 부분이라 일부러 쓰지 않음 |
| `KiwoomConfig::holdings_venue_` | `kt00018`의 `dmst_stex_tp`(국내거래소구분) | 잔고 요청에 보낼 거래소, `KRX` 또는 `NXT` |
| `KiwoomConfig::holdings_position_scope_` | — | 위 거래소 구분이 평가 가격만 정하고 모든 종목이 조회된다는 사용자의 단언(`VALUATION_ONLY`). 명세만으로는 알 수 없어 라이브러리가 추정하지 않음. 틀린 단언이면 잔고를 적게 세어, 이미 한도에 이른 종목의 매수를 허용하거나 매도를 거부·축소할 수 있음. 멤버 문서 참고 |
| `KiwoomConfig::holdings_sellable_scope_` | `trde_able_qty`(매매가능수량)의 범위 | 설정 전에는 매도 가능 수량 검사를 거치는 매도(checked sell)를 거부 |

`OrderSession`의 나머지 값 — `CLOSING_AUCTION`(토스 `CLS`, 미국 주식의 종가 지정가 주문), `AFTER_MARKET`,
`GTP` — 은 한국 증권사 용어와 일대일로 대응시키지 않았습니다. `OrderSession`의 문서를 보세요.

## 더 읽을거리

영어 README의 각 절과 사용자 가이드(영어)를 보세요:

- 설치와 빌드 옵션: 영어 README의 "Prerequisites", "Install".
- 전략 작성과 실거래 프로그램: "Using it", 그리고 가이드의
  [Writing a strategy](doxygen/vignettes/writing-a-strategy.md),
  [Running a strategy live](doxygen/vignettes/running-a-strategy-live.md).
- 장애 후 복구: [Recovering after a crash](doxygen/vignettes/recovering-after-a-crash.md).
- 새 증권사 추가: [The broker port](doxygen/vignettes/the-broker-port.md).

## 면책 조항

<!-- en: "Disclaimer" @ c5d938e -->

algotradelib은 MIT 라이선스로 배포됩니다([LICENSE](LICENSE) 참고). 이 소프트웨어는 어떠한 보증도 없이
있는 그대로 제공되며, 저작자는 거래 손실을 포함해 그 사용으로 생긴 어떠한 손실에도 책임지지 않습니다.
이 저장소의 어떤 내용도 투자 조언이 아닙니다.

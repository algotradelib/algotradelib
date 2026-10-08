# algotradelib <img src="doxygen/figures/logo.png" align="right" height="139" alt="" />

<!-- badges: start -->
[![codecov](https://codecov.io/gh/ygeunkim/algotradelib/graph/badge.svg?token=NH7U8WQOHA)](https://codecov.io/gh/ygeunkim/algotradelib)
![License](https://img.shields.io/badge/license-MIT-blue.svg?style=plastic)
[![Lifecycle: experimental](https://img.shields.io/badge/lifecycle-experimental-orange.svg?style=plastic)](https://lifecycle.r-lib.org/articles/stages.html#experimental)
<!-- badges: end -->

<!-- multilingual: start -->
[![en](https://img.shields.io/badge/lang-en-red.svg?style=plastic)](README.md)
[![ko-kr](https://img.shields.io/badge/lang-ko--kr-green.svg?style=plastic)](README.ko-kr.md)
<!-- multilingual: end -->

<!--
  영어 README.md aceae8b 기준. 요약한 절: 첫 소개와 경고, "Brokers and accounts", "Install",
  "Using it"의 실행 예시, "Documentation", "Acknowledgements", "Disclaimer". 이 절들이
  바뀌면 이 문서도 함께 고칩니다. 키움 설정과 빌드 옵션 같은 자세한 내용은 doxygen/index.md에 있으므로,
  그 페이지의 같은 절이 바뀔 때도 이 문서를 확인합니다.
-->

> 이 문서는 [README](README.md)를 짧게 요약한 안내입니다.
> 두 문서의 내용이 다르면 영어 README를 따르세요.

algotradelib은 한국투자증권 (KIS), 키움증권, 토스증권 Open API로 자동매매 프로그램을 만들 수 있는 C++ 헤더 전용 라이브러리입니다.
전략 코드 하나로 백테스트, 주문 없는 시험 실행, 실거래를 모두 할 수 있고, 증권사를 바꿔도 전략 코드는 그대로 쓸 수 있습니다.

## 유의사항

- KIS와 키움 모의투자 계좌에서는 주문을 넣어 확인했지만, 실전투자 계좌에서 전체 과정을 검증한 적은 없습니다. 모의투자로 먼저 확인하세요.
- KIS와 키움은 기본적으로 모의투자 서버에 연결됩니다. 실전투자는 `KIS_DEMO=0`, `KIWOOM_DEMO=0`처럼 직접 지정해야 합니다 (인증 정보 파일이라면 `"demo": false`). 모의투자 서버는 실전 앱키를 받지 않으므로, 설정을 빠뜨려도 실전 주문은 나가지 않고 요청이 거부됩니다.
- KIS 모의투자 서버는 종목 정보와 휴장일 조회를 지원하지 않습니다 (`EGW02006`).
- 토스증권은 모의투자를 제공하지 않습니다. 토스 인증 정보로 주문하면 실제 계좌에서 거래되므로, 전략은 백테스트나 KIS·키움 모의투자로 먼저 확인하세요.
- 키움은 잔고 조회와 전략 주문에 추가 설정이 필요합니다. 설정하기 전에는 라이브러리가 요청을 보내지 않고 거부합니다.
키움은 장 운영 시간 정보도 제공하지 않아 기본 설정으로는 새로 매수하지 않으며, 예제 프로그램 (`example-live-program`)도 키움에서는 조회만 하고 주문하지 않습니다.
설정 방법은 [문서 첫 페이지](doxygen/index.md)(영어)의 "Brokers and accounts"를 참고하세요.
- SOR (`Venue::COMBINED`)은 KIS와 키움 모두 주문에 쓸 수 있습니다.
다만 KIS 모의투자는 SOR을 받지 않고, 키움 모의투자는 아직 확인하지 못했습니다.
키움은 미체결 조회에서 SOR 주문을 `sor_yn` 값으로 따로 표시한다고 안내하고 있어 라이브러리도 이 값을 읽지만, 두 증권사 모두 SOR 주문이 조회에서 어떻게 보이는지 실제 계좌로 확인하지는 못했습니다.
프로그램이 비정상 종료된 뒤 복구할 때 이 부분이 중요하므로, 당분간은 KRX나 NXT를 지정하세요.

## 지원 증권사

| 증권사 | 지원 기능 | 모의투자 |
| --- | --- | --- |
| KIS | 시세, 장 운영 시간, 종목 정보, 보유 주식, 매수 가능 금액, 주문(스톱지정가 포함), 주문 내역 | 지원 |
| 키움 | 시세, 종목 정보, 매수 가능 금액, 주문(스톱지정가 포함), 주문 내역. 장 운영 시간은 없고, 보유 주식 조회는 추가 설정 필요 | 지원 |
| 토스 | 시세, 장 운영 시간, 종목 정보, 보유 주식, 매수 가능 금액, 주문(스톱지정가 제외), 주문 내역 | 없음 |

## 시작하기

프로젝트에 서브모듈로 추가합니다.

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

예제를 KIS 모의투자로 실행해 보려면 다음과 같이 합니다.

```sh
cmake -S . -B build -DATL_BUILD_EXAMPLES=ON
cmake --build build
export KIS_APP_KEY=... KIS_APP_SECRET=... KIS_CANO=... KIS_DEMO=1
./build/bin/example-live-program kis 005930 # 조회만 하고 주문하지 않음
./build/bin/example-live-program kis 005930 --live # 주문 실행(저널에 기록)
```

환경 변수만으로 실행하면 모의투자 서버가 종목 정보 조회를 지원하지 않아 그 단계에서 멈춥니다.
인증 정보 폴더로 시세와 종목 정보를 따로 조회하는 방법은 예제 파일 (`examples/example-live-program.cpp`) 첫 부분의 설명을 참고하세요.

## 용어

| 라이브러리 | 증권사 문서 |
| --- | --- |
| demo / live | 모의투자 / 실전투자 |
| `Venue::KRX` / `NXT` / `COMBINED` | 한국거래소 / 넥스트레이드 / SOR |
| `OrderSession::REGULAR` | 정규장 |
| `OrderSession::PRE_MARKET` / `AFTER_HOURS` | 장전 시간외 / 장후 시간외 |
| `OrderType::STOP_LIMIT` | 스톱지정가 |
| `HoldingPosition::sellable_quantity_` | 주문가능수량(KIS), 매매가능수량(키움) |
| `BuyingPower::cash_` | 미수없는매수금액(KIS), 100%종목주문가능금액(키움) |

`BuyingPower::cash_`는 미수나 신용 없이 살 수 있는 금액만 나타내므로, 증권사 앱의 주문가능금액보다 작을 수 있습니다.

## 더 알아보기

설치 조건, 빌드 옵션, 실거래 프로그램 구성은 [문서 첫 페이지](doxygen/index.md)(영어)를 참고하세요.
자세한 설명은 [사용자 가이드](doxygen/vignettes/guide.md)(영어)에 있습니다.

## 감사의 말

이 프로젝트는 생성형 AI 도구의 도움을 받아 개발했습니다.

## 라이선스

MIT 라이선스로 배포합니다 ([LICENSE](LICENSE)).
이 라이브러리는 어떠한 보증 없이 제공되며, 이 라이브러리를 사용해 생긴 손해(거래 손실 포함)에 대해 저작자는 책임지지 않습니다.
이 저장소의 어떤 내용도 투자 권유가 아닙니다.

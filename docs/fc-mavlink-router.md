# FC 연동: MAVLink 라우터 구성

FC(Pixhawk 6X, PX4)와 Jetson, 외부 서버 사이의 MAVLink 연결 구성을 기록한다. 깨끗한 Jetson에 워크스페이스를 받아 `install_workspace.sh`만 실행하면 같은 구성이 되고, FC 쪽은 아래 "FC 파라미터"를 한 번 맞춘다.

- 확인: Holybro Pixhawk Jetson Baseboard(Pixhawk 6X, PX4 1.14.3), FC 보드 단독 벤치 시험
- 시스템에 남기는 것은 서비스 파일 1개(`/etc/systemd/system/workspace-mavlink-router.service`)다. 실행 파일은 워크스페이스(`install/bin/mavlink-routerd`), 설정은 데이터 폴더(`dev-workspace_data/config/mavlink-router.conf`)에 있다

## 구성

```text
                         ┌──────────── Jetson (<보드 주소>) ────────────┐
[FC Pixhawk 6X] ─TELEM2─→│ /dev/ttyTHS1 ─→ [mavlink-router] ─UDP─→ <원격 서버>:14540 (서버)
  PX4 1.14.3   (보드 내부)│ 921600, 흐름 제어 끔     │                       │
  sys 1 / comp 1          │                         └─UDP─→ 127.0.0.1:14551 (워크스페이스)
                          └─────────────────────────────────────────────────┘
```

- Holybro Pixhawk Jetson Baseboard는 FC TELEM2와 Jetson `/dev/ttyTHS1`이 보드 안에서 연결되어 있다. 배선할 것이 없다. 보드에 나온 TELEM1, TELEM3는 다른 장치(무선 모뎀 등)용이다
- 시리얼 포트는 한 프로그램만 열 수 있다. mavlink-router가 FC 포트를 혼자 열고, 받은 메시지를 각 출구(UDP)로 복사한다. 출구에서 들어온 메시지는 FC와 다른 출구로 전달한다
- 라우터는 워크스페이스(`workspace.service`)와 독립된 서비스다. 워크스페이스가 내려가도 FC ↔ 서버 링크는 유지된다

| 출구 | 주소 | 용도 |
|---|---|---|
| `flight_controller` | `/dev/ttyTHS1` 921600 | FC. 설치할 때 자동으로 찾는다 |
| `server` | `<원격 서버>:14540` | 원격 서버(지상 GCS 등). **현장 값이라 기본 설정에는 없다.** 프로필의 `mavlink-router.conf`나 데이터 폴더 사본에서 켠다 |
| `local_workspace` | `127.0.0.1:14551` | Jetson 안의 워크스페이스 프로그램 |

`local_workspace`는 `Mode = Normal`이라 라우터가 이 포트 한 곳으로만 보낸다. **이 포트를 여는 프로그램은 하나여야 한다.** 여러 노드가 FC 정보를 써야 하면 MAVLink 담당 노드 하나가 받아서 안에서 나눠 준다.

## 설치

`install_workspace.sh`가 `install/install_mavlink_router.sh`를 호출한다. 혼자 실행해도 된다.

```bash
sudo bash ~/dev-workspace/install/install_mavlink_router.sh
```

상태만 확인(아무것도 바꾸지 않음):

```bash
sudo bash ~/dev-workspace/install/install_mavlink_router.sh --check
```

설치 순서:

| 단계 | 동작 |
|---|---|
| 1. 기존 설치 확인 | 워크스페이스가 아닌 mavlink-router 서비스나 프로세스가 있으면(수동 설치 포함) **건드리지 않고 끝낸다.** 그 설정에 워크스페이스 출구(127.0.0.1:14551)가 없으면 경고만 한다 |
| 2. 실행 파일 확인 | `install/bin/mavlink-routerd`의 sha256 확인 |
| 3. 설정 | `dev-workspace_data/config/mavlink-router.conf`가 **없을 때만** 기본값(`install/config/mavlink-router.conf`)으로 만든다. 이때 `ttyTHS*`, `ttyACM*`, `ttyUSB*`(921600, 57600)에서 FC 하트비트를 찾아 `Device`·`Baud`를 채운다. 못 찾으면 `/dev/ttyTHS1` 921600으로 두고 경고 |
| 4. 시리얼 콘솔 끄기 | `nvgetty`, `serial-getty@<FC 포트>` 비활성화. 워크스페이스 소유자를 `dialout`에 추가 |
| 5. 서비스 | 서비스 파일을 만들어 같으면 그대로(재시작 안 함), 다르면 갱신하고 재시작 |

- 재실행해도 된다. 이미 맞게 설치되어 있으면 아무것도 바꾸지 않아 FC 링크가 끊기지 않는다
- FC 포트를 자동으로 찾으려면 **설치할 때 FC가 켜져 있고 파라미터가 맞아야 한다.** 다른 캐리어 보드에서 못 찾았으면 `dev-workspace_data/config/mavlink-router.conf`의 `Device`를 고친다
- 설정 파일을 지우면 `workspace-start.sh`가 다음 시작 때 기본값(`/dev/ttyTHS1`)으로 다시 만든다. 포트를 다시 찾게 하려면 지우고 `install_mavlink_router.sh`를 실행한다
- 서비스는 워크스페이스 소유자 계정(+`dialout` 그룹)으로 돈다

### 이미 수동으로 설치된 라우터가 있을 때

설치 스크립트는 워크스페이스 밖에서 설치한 mavlink-router(서비스나 프로세스)를 감지하면 건드리지 않고 건너뛴다. 그 설정에 워크스페이스 출구(`127.0.0.1:14551`)가 없으면 경고한다. 그대로 쓰려면 그 설정에 아래 출구를 추가한다.

```ini
[UdpEndpoint local_workspace]
Mode = Normal
Address = 127.0.0.1
Port = 14551
```

워크스페이스 구성으로 바꾸려면 수동 설치 서비스를 끄고(예: `sudo systemctl disable --now mavlink-router`) `install_mavlink_router.sh`를 실행한다. 두 서비스가 함께 돌면 FC 포트를 두고 충돌한다.
## 구성 요소

| 항목 | 값 |
|---|---|
| FC | Pixhawk 6X (Holybro Jetson Baseboard 내장), PX4 1.14.3 |
| Jetson | Orin NX, L4T R36.5 (Ubuntu 22.04) |
| mavlink-router | github.com/mavlink-router/mavlink-router `5e38c7a` (2026-10-23), aarch64 빌드 |
| 실행 파일 | `install/bin/mavlink-routerd`, sha256 `8e2cacdc0b7a0daddeb409ca8f155bf530437c91f14b9a626fc412fe0ba08347`. 의존: glibc, libstdc++ |
| 설정 | 기본값 `install/config/mavlink-router.conf` → 현장 `dev-workspace_data/config/mavlink-router.conf`, 추가 조각 폴더 `dev-workspace_data/config/mavlink-router.d/` |
| 서비스 | `install/systemd/workspace-mavlink-router.service` → `/etc/systemd/system/workspace-mavlink-router.service` |

버전을 올릴 때: 새로 빌드(PC의 jetson-dev-env 또는 Jetson) → 이 문서의 "연결 확인"으로 시험 → `install/bin/mavlink-routerd`와 `install_mavlink_router.sh`의 `BIN_SHA256`을 함께 바꾼다.

## FC 파라미터

FC 안에 저장되는 값이라 Jetson 설치로는 바뀌지 않는다. FC를 새로 쓰거나 초기화했으면 맞춘다. QGC → Vehicle Setup → Parameters에서 이름으로 검색해 바꾼 뒤 Tools → Reboot Vehicle.

| 파라미터 | 값 | 의미 |
|---|---|---|
| `MAV_1_CONFIG` | `TELEM 2` (102) | MAVLink 두 번째 인스턴스를 TELEM2에 |
| `MAV_1_MODE` | `Onboard` (2) | 컴패니언용 메시지 세트 |
| `MAV_1_RATE` | 0 | 전송 속도 자동 |
| **`MAV_1_FLOW_CTRL`** | **`Force off` (0)** | **하드웨어 흐름 제어 끔. 필수** |
| `SER_TEL2_BAUD` | 921600 | 라우터 `Baud`와 같아야 한다 |
| `UXRCE_DDS_CFG` | `Disabled` (0) | TELEM2를 DDS가 쓰지 않게 |
| `MAV_0_CONFIG` | `TELEM 1` (101) | 기본값. 외부 TELEM1 (57600, Normal) |
| `MAV_2_CONFIG` | `Ethernet` (1000) | 이더넷 MAVLink도 켜져 있음. 지금 구성에서는 쓰지 않음 |
| `MAV_SYS_ID` / `MAV_COMP_ID` | 1 / 1 | FC 식별 |

- `MAV_1_CONFIG`를 바꾼 뒤 재부팅해야 `SER_TEL2_BAUD`가 목록에 나온다
- `MAV_1_FLOW_CTRL`이 켜져 있으면(기본 `Auto`에서 ON으로 잡혔다) FC가 CTS 신호를 기다리며 **한 바이트도 보내지 않는다**. 라우터는 흐름 제어를 쓰지 않는다

## 라우터 설정

`install/config/mavlink-router.conf`(기본값)의 설정 부분:

```ini
[General]
TcpServerPort = 0
ReportStats = true
MavlinkDialect = common

[UartEndpoint flight_controller]
Device = /dev/ttyTHS1
Baud = 921600

# 원격 서버: 기본은 주석(현장 값). 프로필에서 켠다
#[UdpEndpoint server]
#Mode = Normal
#Address = <원격 서버>
#Port = 14540

[UdpEndpoint local_workspace]
Mode = Normal
Address = 127.0.0.1
Port = 14551
```

- 현장 값(서버 주소 등)은 프로필의 `mavlink-router.conf`(설치할 때 `--profile`)나 `<데이터>/config/mavlink-router.conf`에서 바꾸고 `sudo systemctl restart workspace-mavlink-router`. 재시작하는 1~2초 동안 모든 출구가 끊긴다
- `ReportStats = true`: 출구별 수신·송신 통계를 1초마다 journal에 남긴다. 연결 확인에 쓴다
- `TcpServerPort = 0`: TCP 서버 끔. 로컬과 LTE 구간 모두 UDP를 쓴다(MAVLink는 ACK·재전송을 스스로 하고, TCP는 지연이 쌓이면 최신 값이 늦게 온다)
- `#`으로 시작하는 줄은 주석이다. 줄 끝 주석은 쓸 수 없다

## 연결 확인

아래 명령의 서비스 이름은 워크스페이스 설치 기준(`workspace-mavlink-router`)이다. 수동 설치 보드는 `mavlink-router`.

**1. 라우터 통계 (FC → Jetson)**

```bash
journalctl -u workspace-mavlink-router --no-pager --since "-5s" | grep -A9 "flight_controller {"
```

`Received messages`의 `Total`이 계속 늘면 정상이다. 2026-11-10 기준 약 300개/초. `Total: 0`이 이어지면 FC가 보내지 않는 것이다(아래 문제 해결).

**2. FC 쪽 상태 (QGC → Analyze Tools → MAVLink Console)**

```text
mavlink status
```

`/dev/ttyS4 @921600`, `mode: Onboard` 인스턴스가 TELEM2다. `flow control: OFF`이고 `tx`가 0보다 커야 한다.

**3. 워크스페이스에서 수신·명령**

워크스페이스에서는 **FC 워커**(장치 워커로 따로 만든다) 가 이 출구를 맡는다. 텔레메트리 해석·지표·이벤트, Jetson에서 FC로 명령(모드, 메시지 주기, 일반 COMMAND_LONG), 화면 패널 "FC 텔레메트리"를 제공한다. 아래 pymavlink 예와 `test/fc_test.py`는 워커를 정지한 상태에서 쓰는 직접 확인용이다(출구는 한 프로그램만 쓸 수 있음).

워크스페이스 프로그램은 `udpin:127.0.0.1:14551`로 받고, `source_system=1`, `source_component=191`(ONBOARD_COMPUTER)로 보낸다. pymavlink 최소 예:

```python
from pymavlink import mavutil
m = mavutil.mavlink_connection("udpin:127.0.0.1:14551",
                               source_system=1, source_component=191)
m.wait_heartbeat(timeout=10)                     # 서버 쪽 하트비트도 오므로 실제로는 autopilot 필드로 FC를 고른다
m.target_system, m.target_component = 1, 1
m.mav.command_long_send(1, 1, mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL, 0,
                        mavutil.mavlink.MAVLINK_MSG_ID_ATTITUDE, 20000, 0, 0, 0, 0, 0)
print(m.recv_match(type="COMMAND_ACK", blocking=True, timeout=3))
```

한 번에 확인하는 시험 도구: [test/fc_test.py](../test/fc_test.py). 하트비트 → 스트림 주기 → `SET_MESSAGE_INTERVAL` 명령·ACK를 차례로 보고, `--arm`이면 ARM/DISARM까지 한다(모터 없는 보드에서만). pymavlink가 필요하다(`pip install --user pymavlink`).

```bash
python3 ~/dev-workspace/test/fc_test.py
```

2026-11-10 시험 결과(보드 단독, 이 도구로):

| 항목 | 결과 |
|---|---|
| 하트비트 | FC sys 1 / comp 1, PX4, Quadrotor |
| 스트림 | ATTITUDE 100Hz, HIGHRES_IMU 50Hz, ATTITUDE_QUATERNION 50Hz, LOCAL_POSITION_NED 30Hz 외 |
| `SET_MESSAGE_INTERVAL` (ATTITUDE 50Hz) | ACK ACCEPTED, 50Hz 측정. 시험 후 기본값으로 되돌림 |
| ARM | ACK TEMPORARILY_REJECTED (RC·배터리·GPS 없음, 정상) |

- 메시지 주기 변경은 FC 쪽 설정이라 모든 출구(서버 포함)에 같이 적용된다
- PX4는 정수 파라미터 값을 float 필드에 바이트 그대로 담아 보낸다(`PARAM_VALUE.param_type`이 정수형일 때). `int(param_value)`로 읽으면 0이 나온다. `struct.unpack("<i", struct.pack("<f", v))`로 푼다

## 문제 해결

| 증상 | 원인 | 조치 |
|---|---|---|
| 라우터 `flight_controller` `Total: 0`, CRC 오류도 0 | FC가 송신하지 않음 | MAVLink Console `mavlink status`. `flow control: ON`, `tx 0`이면 `MAV_1_FLOW_CTRL` = Force off 후 재부팅 |
| `mavlink status`에 TELEM2 인스턴스가 없음 | `MAV_1_CONFIG` 미설정 또는 재부팅 안 함 | 파라미터 확인 후 재부팅 |
| 라우터 CRC 오류가 계속 증가 | baud 불일치 또는 DDS가 같은 포트를 씀 | `SER_TEL2_BAUD`, `UXRCE_DDS_CFG` 확인 |
| 재부팅 직후 CRC 오류·순서 손실이 조금 생김 | FC 부팅 중 | 이후 늘지 않으면 정상 |
| 설치할 때 "no FC heartbeat found" | FC 꺼짐, 파라미터 미설정, 다른 프로그램이 포트를 엶 | FC 확인 후 설정 파일을 지우고 다시 설치, 또는 `Device`를 직접 고침 |
| 설치가 "already installed outside the workspace"로 끝남 | 수동 설치 라우터가 있음 | 그대로 쓰거나(워크스페이스 출구 추가), 위 "이미 수동으로 설치된 라우터가 있을 때"대로 끄고 다시 설치 |
| 워크스페이스 프로그램이 포트를 열지 못함 | 14551을 다른 프로그램이 이미 엶 | `ss -uanp \| grep 14551` |
| 테스트 프로그램이 FC 포트를 직접 열면 데이터가 안 오거나 라우터에 손실이 생김 | 라우터와 같은 포트를 나눠 읽음 | 포트를 직접 열지 말고 `local_workspace` 출구를 쓴다 |
| 라우터 시작 로그 `Error while trying to write serial port latency` | tegra UART 드라이버가 latency 설정을 받지 않는 것으로 보임 | 그 뒤 `Opened UART`가 나오고 수신되면 무시해도 된다 |

## 메시지 ID

라우터는 메시지의 대상 ID로 목적지를 고른다. 대상이 없는 메시지(하트비트, 상태)는 모든 출구로 간다. 참여자끼리 ID가 겹치면 응답이 엉뚱한 곳으로 간다.

| 참여자 | sysid / compid |
|---|---|
| FC | 1 / 1 |
| 워크스페이스(컴패니언) | 1 / 191 |
| 서버·GCS | 미정 (QGC 기본 255 / 190) |

## 미정 사항

- **명령 권한과 우선순위**: 서버(GCS)와 워크스페이스가 둘 다 FC에 명령할 수 있다. PX4는 보낸 쪽을 구분하지 않고 나중 명령을 따른다. 비상 명령(Kill, Land, RTL)은 GCS → FC로 항상 통과하게 하고, 그 밖의 명령을 어디서 중재할지(워크스페이스 관문, 라우터 필터, 명령 종류별 분리) 정해야 한다
- **`server` 출구 유지 여부**: FC 원본을 서버로 바로 보내는 경로를 모니터링용으로 둘지, 외부는 워크스페이스를 거치게 할지
- **LTE 단절 시 동작**: FC failsafe(데이터링크 끊김)와 워크스페이스 동작

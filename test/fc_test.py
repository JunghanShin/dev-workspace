#!/usr/bin/env python3
"""FC 연동 시험: Jetson(컴패니언) <-> mavlink-router <-> FC(PX4) 경로 확인 (docs/fc-mavlink-router.md "확인").

라우터의 워크스페이스 출구(127.0.0.1:14551)로 붙어 컴패니언(sysid 1, compid 191)처럼 동작한다.

    python3 ~/dev-workspace/test/fc_test.py          하트비트, 스트림 주기, 명령·ACK
    python3 ~/dev-workspace/test/fc_test.py --arm    + ARM → DISARM (모터 없는 보드에서만)

  [1] FC 하트비트 기다림(10초): sysid/compid, autopilot, 기체 type, arm 상태
  [2] 3초 동안 FC가 보내는 메시지와 주기, ATTITUDE, 배터리 전압
  [3] MAV_CMD_SET_MESSAGE_INTERVAL로 ATTITUDE 50 Hz 요청 → ACK와 실제 주기
  [4] --arm: ARM/DISARM 명령과 ACK, 거부 사유(STATUSTEXT)

필요: pymavlink (pip install --user pymavlink). 라우터의 출구 하나는 받는 쪽이 하나뿐이라
같은 포트(14551)를 쓰는 다른 프로그램이 있으면 끄고 실행한다.
"""
import sys, time
from collections import Counter
from pymavlink import mavutil

URL = "udpin:127.0.0.1:14551"
DO_ARM = "--arm" in sys.argv

m = mavutil.mavlink_connection(URL, source_system=1, source_component=191)

def send_hb():
    m.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_ONBOARD_CONTROLLER,
                         mavutil.mavlink.MAV_AUTOPILOT_INVALID, 0, 0, 0)

# 1) wait for the autopilot heartbeat (ignore GCS/other heartbeats)
print("[1] waiting for FC heartbeat on", URL)
t0 = time.time()
fc = None
while time.time() - t0 < 10:
    hb = m.recv_match(type="HEARTBEAT", blocking=True, timeout=1)
    if hb and hb.autopilot != mavutil.mavlink.MAV_AUTOPILOT_INVALID:
        fc = hb
        break
if not fc:
    print("    no FC heartbeat"); sys.exit(1)
m.target_system = fc.get_srcSystem(); m.target_component = fc.get_srcComponent()
ap = mavutil.mavlink.enums["MAV_AUTOPILOT"][fc.autopilot].name
vt = mavutil.mavlink.enums["MAV_TYPE"][fc.type].name
armed = bool(fc.base_mode & mavutil.mavlink.MAV_MODE_FLAG_SAFETY_ARMED)
print(f"    FC sys={m.target_system} comp={m.target_component} {ap} {vt} armed={armed}")
send_hb()

# 2) what is streaming (3 s)
print("[2] message stream for 3 s")
cnt = Counter(); t0 = time.time()
while time.time() - t0 < 3:
    msg = m.recv_match(blocking=True, timeout=0.5)
    if msg and msg.get_srcSystem() == m.target_system and msg.get_srcComponent() == m.target_component:
        cnt[msg.get_type()] += 1
print("    " + ", ".join(f"{k}:{v/3:.0f}Hz" for k, v in cnt.most_common(12)))
att = m.recv_match(type="ATTITUDE", blocking=True, timeout=3)
if att:
    print(f"    ATTITUDE roll={att.roll:.3f} pitch={att.pitch:.3f} yaw={att.yaw:.3f} rad")
st = m.recv_match(type="SYS_STATUS", blocking=True, timeout=3)
if st:
    print(f"    SYS_STATUS battery={st.voltage_battery/1000:.2f} V")

def command(cmd, *params, timeout=3):
    p = list(params) + [0] * (7 - len(params))
    m.mav.command_long_send(m.target_system, m.target_component, cmd, 0, *p)
    t0 = time.time(); texts = []
    while time.time() - t0 < timeout:
        msg = m.recv_match(type=["COMMAND_ACK", "STATUSTEXT"], blocking=True, timeout=0.5)
        if not msg: continue
        if msg.get_type() == "STATUSTEXT":
            texts.append(msg.text)
        elif msg.command == cmd:
            r = mavutil.mavlink.enums["MAV_RESULT"][msg.result].name
            print(f"    ACK {r}")
            time.sleep(0.5)
            while (s := m.recv_match(type="STATUSTEXT", blocking=False)):
                texts.append(s.text)
            for t in texts: print(f"    STATUSTEXT: {t}")
            return msg.result
    print("    no ACK (timeout)")
    for t in texts: print(f"    STATUSTEXT: {t}")
    return None

# 3) command/response: set ATTITUDE to 50 Hz and verify
print("[3] MAV_CMD_SET_MESSAGE_INTERVAL ATTITUDE -> 50 Hz")
send_hb()
command(mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL,
        mavutil.mavlink.MAVLINK_MSG_ID_ATTITUDE, 20000)
n = 0; t0 = time.time()
while time.time() - t0 < 2:
    if m.recv_match(type="ATTITUDE", blocking=True, timeout=0.5): n += 1
print(f"    measured ATTITUDE {n/2:.0f} Hz")

# 4) arm/disarm (bench board only, no motors)
if DO_ARM:
    print("[4] ARM")
    send_hb()
    r = command(mavutil.mavlink.MAV_CMD_COMPONENT_ARM_DISARM, 1)
    if r == mavutil.mavlink.MAV_RESULT_ACCEPTED:
        time.sleep(1)
        print("    DISARM")
        command(mavutil.mavlink.MAV_CMD_COMPONENT_ARM_DISARM, 0)
print("done")

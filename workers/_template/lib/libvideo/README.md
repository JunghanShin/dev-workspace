# libvideo: 장치 워커 안의 영상 송출과 프레임 탭

장치 워커(siyi, ximea 등)가 설정 `streams`의 송출마다 **자식 프로세스**(gst-launch·ffmpeg)를 띄우고 감시하는 모듈. 탭(`tap`)은 카메라 RTSP를 풀어 원본 프레임을 같은 보드의 **프레임 버스**로 낸다(인식 같은 영상 처리 워커가 받아 감). 워크스페이스에 의존하지 않는다. 쓰는 워커마다 `lib/libvideo/`에 사본을 두고, 원본은 `workers/_template/lib/libvideo`.

| 파일 | 내용 |
|---|---|
| `include/video/stream.hpp` | `StreamConfig`(송출 설정), `parse_streams`(설정 읽기·검증), `Stream`(송출 하나: 자식 시작·진행 확인·멈춤 판정·다시 시작·켜기/끄기), `Streams`(송출 전체: poll 루프 연결, `stream_start`/`stream_stop` 명령, 상태·지표) |
| `include/video/pipeline.hpp` | 단계별 명령(순수 함수): 입력(rtsp / framebus) → 처리(relay / encode: NVDEC → 시각 표시 → NVENC) → 출력(rtsp_udp·rtsp_tcp·srt) |
| `src/feeder.*` | 입력 framebus: 프레임 버스의 최신 프레임을 fps 간격으로 gst 표준 입력에 넘기는 스레드 |
| `include/video/tap.hpp` | `TapConfig`·`parse_tap`(설정 칸 `tap`), `gst_tap_command`(RTSP → NVDEC → nvvidconv → 원본 BGRx·RGBA·GRAY8 → 표준 출력), `Tap`(자식 감시·다시 시작, 받기 스레드가 프레임을 프레임 버스 슬롯에 바로 읽어 넣음, 시각 = 받은 순간의 CLOCK_MONOTONIC), `TapModule`(워커 모듈 `tap`, 소켓 기본값 `default_socket(워커 id, "frames")`, 지표 `tap_link·fps·frames·dropped·consumers·restarts`). 송출과 따로 띄워 서로 영향이 없다(영상을 한 번 더 풂) |

장치 워커에서:

```cmake
add_subdirectory(lib/libworker)
add_subdirectory(lib/libframebus)
add_subdirectory(lib/libvideo)
target_link_libraries(my-worker PRIVATE video)
```

```cpp
video::StreamConfig d;              // 이 장치의 입력 기본값
d.source = "rtsp://<카메라>:8554/main.264";
video::Streams streams;
streams.configure(video::parse_streams(cfg["streams"], d));
// poll 루프: streams.add_pollfds(fds) → poll → streams.on_poll(fds, now); streams.on_timer(now)
// 명령: if (streams.handle(cmd, args, &reply)) ...; get_status에 streams.status(), heartbeat에 streams.metrics()
// 종료: streams.stop()
```

탭을 붙일 때(장치 모듈이 입력 기본값을 준다):

```cpp
host.add<video::TapModule>([&cam] { video::TapConfig t; t.source = cam.rtsp_url(); return t; });
// 설정 {"tap": {"enabled": true, "width": 1280, "height": 720, "pixel": "BGRx"}}
```

시험: 이 폴더에서 `jetson-build --native --test`(가짜 ffmpeg·gst로 relay·encode·두 송출 독립·켜기/끄기·framebus, 가짜 gst로 탭: 프레임 버스 소비자가 받음·자식이 끝나면 다시 띄움·소비자는 붙은 채).

설정 폼: `stream-schema.json`은 송출 한 항목의 화면 설정 폼 스키마 원본이다(워크스페이스 design.md 6.10). 송출하는 장치 워커는 자기 `worker-types.json`의 `config_schema` 안 `streams` 목록 `fields`에 이 내용을 그대로 넣는다. `StreamConfig` 키를 바꾸면 이 파일도 같이 고친다.

송출 모듈(워커 모듈 구조, 워크스페이스 design.md 6.6.3): `video/module.hpp`의 `video::StreamsModule`(모듈 이름 `stream`). 장치 워커가 `host.add<video::StreamsModule>([&cam] { return cam.stream_defaults(); })`로 붙인다(입력 기본값은 장치 모듈이 줌). 설정 `{"stream": {"streams": [...]}}`, 명령 `stream.start|stop {name}`, 상태 `get_status` → `data.stream`(송출 목록), 지표 `stream_<이름>_link|fps|restarts`. `fps`는 ffmpeg 진행 보고의 `frame=` 변화로 센 최근 3 s 값(0.3.1). ffmpeg의 `fps=`는 시작부터의 평균이라 다시 시작한 직후 한참 낮게 나와 쓰지 않는다. 프레임이 2 s 넘게 안 늘면 0

## 입출력 정의 조각 (워크스페이스 design.md 6.6.7.1)

이 라이브러리 모듈이 내는 상태·지표·명령은 조각 정의 `io/stream.json`(모듈 `stream`), `io/tap.json`(모듈 `tap`)에 있다. 이 라이브러리를 쓰는 워커는 칸을 다시 적지 않고 `io/self.json`에 `"include": ["lib/libvideo/io/stream.json", "lib/libvideo/io/tap.json"]`(쓰는 모듈만)로 가져온다. 모듈을 고쳐 내보내는 칸이 바뀌면 조각도 같이 고친다(출력 대조가 잡는다).

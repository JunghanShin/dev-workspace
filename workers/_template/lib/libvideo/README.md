# libvideo: 장치 워커 안의 영상 송출

장치 워커(siyi, ximea 등)가 설정 `streams`의 송출마다 **자식 프로세스**(gst-launch·ffmpeg)를 띄우고 감시하는 모듈. 워크스페이스에 의존하지 않는다. 쓰는 워커마다 `lib/libvideo/`에 사본을 두고, 원본은 `workers/_template/lib/libvideo`.

| 파일 | 내용 |
|---|---|
| `include/video/stream.hpp` | `StreamConfig`(송출 설정), `parse_streams`(설정 읽기·검증), `Stream`(송출 하나: 자식 시작·진행 확인·멈춤 판정·다시 시작·켜기/끄기), `Streams`(송출 전체: poll 루프 연결, `stream_start`/`stream_stop` 명령, 상태·지표) |
| `include/video/pipeline.hpp` | 단계별 명령(순수 함수): 입력(rtsp / framebus) → 처리(relay / encode: NVDEC → 시각 표시 → NVENC) → 출력(rtsp_udp·rtsp_tcp·srt) |
| `src/feeder.*` | 입력 framebus: 프레임 버스의 최신 프레임을 fps 간격으로 gst 표준 입력에 넘기는 스레드 |

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

시험: 이 폴더에서 `jetson-build --native --test`(가짜 ffmpeg·gst로 relay·encode·두 송출 독립·켜기/끄기·framebus).

설정 폼: `stream-schema.json`은 송출 한 항목의 화면 설정 폼 스키마 원본이다(워크스페이스 design.md 6.10). 송출하는 장치 워커는 자기 `worker-types.json`의 `config_schema` 안 `streams` 목록 `fields`에 이 내용을 그대로 넣는다. `StreamConfig` 키를 바꾸면 이 파일도 같이 고친다.

송출 모듈(워커 모듈 구조, 워크스페이스 design.md 6.6.3): `video/module.hpp`의 `video::StreamsModule`(모듈 이름 `stream`). 장치 워커가 `host.add<video::StreamsModule>([&cam] { return cam.stream_defaults(); })`로 붙인다(입력 기본값은 장치 모듈이 줌). 설정 `{"stream": {"streams": [...]}}`, 명령 `stream.start|stop {name}`, 상태 `get_status` → `data.stream`(송출 목록), 지표 `stream_<이름>_link|fps|restarts`.

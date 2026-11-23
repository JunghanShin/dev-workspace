// 사용자·토큰 설정 명령(보드에서 사람이 한 번 실행). 비밀번호는 명령 줄로 받지 않는다(ps·셸 기록에 남지 않게).
//
//   WORKSPACE_DATA=<데이터> gateway user add <이름> <viewer|operator|maintainer>   비밀번호 두 번(터미널이면 안 보이게, 아니면 표준 입력 두 줄)
//   gateway user passwd <이름> · gateway user del <이름> · gateway user list
//   gateway token add <사용자> <토큰 이름>    새 토큰을 한 번만 출력(파일에는 sha256만)
//   gateway token del <사용자> <토큰 이름>
//
// 파일: <데이터>/config/users.json(0600, 임시 파일 → fsync → rename). 게이트웨이는 다시 시작하지 않아도 1 s 안에 읽는다.
// WORKSPACE_AUTH_ITERATIONS: 새 비밀번호의 PBKDF2 반복 수(기본 200000, 시험·느린 보드용으로만 줄임)
#pragma once

namespace gw {

// argv[1]이 "user" 또는 "token"이면 처리하고 종료 코드(0 성공, 1 실패, 2 사용법)를, 아니면 -1
int users_cli(int argc, char** argv);

}  // namespace gw

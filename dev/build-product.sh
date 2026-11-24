#!/bin/bash
# 제품 묶음 만들기(워크스페이스 design.md 11.4). 개발 부품을 걷어내고 장비에 올릴 결과물 하나를 만든다.
# 화면에서는 설정 > 워크스페이스 > "제품 빌드"(보드가 스스로 빌드하고 다운로드). 이 명령은 PC에서 같은 일을 한다.
#
#   dev/build-product.sh <프로필>              install/profiles/<프로필>/product.json 기준 → dist/<이름>-<버전>.tar.gz
#   dev/build-product.sh <프로필> --check      빌드 전 점검만(깃·워커 확정본·컴파일 도구·빈 공간)
#   dev/build-product.sh <프로필> --allow-dirty  커밋 안 한 변경이 있어도(시험용)
#
# Python 부분(지금은 없음, 예전 logger 등)은 aarch64에서 Nuitka로 컴파일한다: PC면 dev/board.env의 보드 임시 폴더,
# 보드에서 실행하면 그 자리에서.
# 장비 설치: tar xzf <묶음> && sudo bash <폴더>/install.sh --owner <계정>  (--rollback, --status, --uninstall)
exec python3 "$(dirname "$(readlink -f "$0")")/build_product.py" "$@"

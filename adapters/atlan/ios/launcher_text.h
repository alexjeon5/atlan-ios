// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

// Every word the launcher shows, in one place, so wording changes touch only this file.
namespace launcher_text {
static NSString* const app_name = @"Atlan iOS Launcher";

// Shared
static NSString* const settings = @"설정";
static NSString* const map_data = @"지도 데이터";
static NSString* const app_row = @"아틀란 앱";
static NSString* const map_row = @"지도";
static NSString* const safety_row = @"안전운전";
static NSString* const ready = @"준비됨";
static NSString* const present = @"확인됨";
static NSString* const absent = @"없음";
static NSString* const no_package = @"패키지 없음";
static NSString* const unknown = @"—";
static NSString* const pick_folder = @"폴더 선택";
static NSString* const recheck = @"다시 확인";

// First run
static NSString* const welcome_title = @"ATLAN 폴더를\n추가해 주세요";
static NSString* const welcome_body = @"보유한 ATLAN 폴더를 그대로 사용합니다. 파일을 복사하지 않고 원래 위치에서 읽습니다.";
static NSString* const welcome_way1 = @"방법 1 · 이 %@에 넣기";
static NSString* const welcome_way1_body = @"파일 앱의 나의 %@ › Atlan에 ATLAN 폴더를 통째로 넣으세요";
static NSString* const welcome_way2 = @"방법 2 · 다른 위치의 폴더 사용";
static NSString* const welcome_way2_body = @"iCloud Drive 등 다른 위치에 있는 ATLAN 폴더를 선택하세요";
static NSString* const welcome_layout = @"ATLAN 폴더 구성";
static NSString* const welcome_layout_body = @"SD 카드 전체가 아니라 그 안의 ATLAN 폴더만 넣으세요";
static NSString* const welcome_package = @"앱 패키지는 APP 폴더의 atlan.dat 또는 APK 파일에서 자동으로 찾습니다";

// Auto start question
static NSString* const auto_title = @"아틀란을 자동으로\n시작할까요?";
static NSString* const auto_body = @"앱을 열면 이 화면을 거치지 않고 내비게이션을 바로 시작합니다.";
static NSString* const auto_yes = @"자동 시작 사용";
static NSString* const auto_no = @"사용 안 함";
static NSString* const auto_return = @"돌아오는 방법";
static NSString* const auto_return_body = @"내비게이션에서 FINE DRIVE 버튼을 누르면 이 화면으로 돌아옵니다";
static NSString* const auto_later = @"나중에 변경";
static NSString* const auto_later_body = @"설정의 일반 탭에서 언제든지 변경할 수 있습니다";

// Several ATLAN folders
static NSString* const choose_title = @"사용할 ATLAN 폴더를\n선택해 주세요";
static NSString* const choose_body = @"ATLAN 폴더가 %lu개 있습니다. 설정의 지도 데이터 탭에서 다른 폴더로 변경할 수 있습니다.";
static NSString* const choose_use = @"선택한 폴더 사용";
static NSString* const atlan_folders = @"ATLAN 폴더";
static NSString* const chosen_folder = @"선택한 폴더";
static NSString* const applies_next_start = @"변경한 폴더는 다음 내비게이션 시작부터 적용됩니다";
static NSString* const location_in_documents = @"앱 Documents/%@";

// Start
static NSString* const start = @"내비게이션 시작";
static NSString* const resume = @"내비게이션으로 돌아가기";
static NSString* const folder_missing = @"폴더 없음";
static NSString* const folders_missing = @"%@ 폴더 없음";
static NSString* const package_missing = @"앱 패키지 없음";
static NSString* const blocked_body = @"ATLAN 폴더를 찾을 수 없습니다. 지도 파일이 있는 폴더를 선택해 주세요.";
static NSString* const pick_map_folder = @"지도 폴더 선택";

// Settings tabs
static NSString* const tab_general = @"일반";
static NSString* const tab_map = @"지도 데이터";
static NSString* const tab_versions = @"버전 정보";
static NSString* const tab_developer = @"개발자";
static NSString* const back = @"시작 화면으로";

// General
static NSString* const section_start = @"시작";
static NSString* const auto_start = @"아틀란 자동 시작";
static NSString* const auto_start_detail = @"앱을 열면 내비게이션을 바로 시작합니다. FINE DRIVE 버튼을 누르면 이 화면으로 돌아옵니다.";
static NSString* const section_screen = @"화면";
static NSString* const appearance = @"화면 모드";
static NSString* const appearance_detail = @"시스템 설정을 따르거나 직접 선택합니다";
static NSString* const appearance_system = @"시스템";
static NSString* const appearance_light = @"라이트";
static NSString* const appearance_dark = @"다크";
static NSString* const notch = @"노치 여백";
static NSString* const notch_detail = @"아이폰에서 노치 쪽 화면을 비워 둡니다";
static NSString* const section_sound = @"소리";
static NSString* const duck = @"안내 음성 때 다른 앱 소리 줄이기";
static NSString* const duck_detail = @"음악은 그대로 재생되고 안내가 나올 때만 작아집니다";

// Map data
static NSString* const location = @"위치";
static NSString* const pick_other = @"다른 폴더 선택";
static NSString* const use_documents = @"앱 폴더 사용";
static NSString* const status = @"상태";
static NSString* const map_footnote = @"파일 앱이나 iCloud Drive의 폴더를 선택할 수 있습니다. 파일을 복사하지 않고 원래 위치에서 읽습니다.";

// Versions
static NSString* const nav_software = @"내비게이션 S/W";
static NSString* const nav_db = @"내비게이션 DB";
static NSString* const nav_db_detail = @"지도 · 경로 · 검색";
static NSString* const safety_db = @"안전운전정보 DB";
static NSString* const safety_db_detail = @"메인 · 부가";
static NSString* const verified = @"검증된 버전";
static NSString* const unverified = @"검증되지 않은 버전";
static NSString* const in_use = @"사용 중";
static NSString* const versions_footnote = @"목록에 없는 버전도 실행할 수 있지만 지원하지 않는 기능에서 중단될 수 있습니다.";
static NSString* const verified_footnote = @"%@ %@에서 실행을 확인한 버전입니다";

// Developer
static NSString* const section_logs = @"로그";
static NSString* const section_location = @"위치";
static NSString* const pin_location = @"위치 고정";
static NSString* const pin_location_detail = @"실제 위치 대신 아래 좌표를 내비게이션에 보냅니다";
static NSString* const coordinate = @"좌표";
static NSString* const coordinate_detail = @"위도, 경도";
static NSString* const course = @"진행 방향";
static NSString* const course_detail = @"북쪽 0°, 시계 방향";
static NSString* const session_log = @"세션 로그 저장";
static NSString* const session_log_detail = @"Documents/logs · 최근 5개 보관";
static NSString* const perf_log = @"성능 로그";
static NSString* const perf_log_detail = @"Documents/perf.log · 10초마다 기록";
static NSString* const net_log = @"네트워크 로그";
static NSString* const net_log_detail = @"Documents/net.log";
static NSString* const profiler = @"함수 프로파일러";
static NSString* const profiler_detail = @"Documents/profile 폴더에 함수별 기록";
}

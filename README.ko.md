# 모니터 밝기 조정 (Bloom 애드온)

[English](README.md)

[Bloom](https://github.com/hyeon1035298/Bloom) 런처의 애드온으로, Windows 11 작업표시줄에서 모니터 밝기를 맞춥니다. 시계 옆의 작은 모니터 아이콘이 밝기를 보이고, 그 위에서 휠을 굴리면 모든 모니터가 함께 바뀌며, 누르면 모니터마다 슬라이더가 있는 창이 열립니다.

- 모니터마다 슬라이더. 외부 모니터는 DDC/CI, 노트북 내장 화면은 WMI로 바꿉니다
- 작업표시줄 아이콘 위 휠로 모든 모니터를 같은 만큼
- 모니터별 범위 맞추기: 창의 0과 100이 가리킬 실제 밝기를 정해 밝기가 다른 모니터를 나란히 움직입니다
- 모두 함께: 슬라이더 하나를 움직이면 다른 모니터도 같은 값으로
- 단축키(처음에는 Ctrl+Alt+↑ / ↓), 설정에서 바꿀 수 있음
- 유휴 시 어둡게: 정한 시간 동안 입력이 없으면 정한 밝기로 낮추고 입력이 오면 되돌립니다. 동영상처럼 화면을 켜 두는 프로그램이 있으면 어둡게 하지 않습니다
- DDC/CI 대비와 모니터 하나 끄기, 마우스를 움직일 때까지 모든 화면을 끄는 버튼

기본 Windows 11 작업표시줄과 [PillTaskbar](https://github.com/hyeon1035298/PillTaskbar_BloomAddon) 어느 쪽에서도 똑같이 작동하며 PillTaskbar가 없어도 됩니다. 메모리 정리 애드온도 켜 두면 밝기, RAM, 시계 순서로 나란히 놓이고, 두 창은 한 번에 하나만 열립니다.

## 작동 방식

PillTaskbar의 애드온 런타임과 호스트가 `mod/`의 모드를 탐색기에 올립니다. 호스트는 모드가 작업표시줄 XAML에 닿는 데 쓰는 작업표시줄 함수를 찾고, 모드가 쓰는 Windhawk API를 제공하는 작은 런타임을 넣고, 계속 지켜봅니다. 호스트는 Bloom이 띄우는 별도의 작은 프로세스입니다.

탐색기 안에서 모드는 트레이에 아이콘을 넣고, 필요할 때만 창을 열고, 모니터를 맡는 작업 스레드 하나를 둡니다. DDC/CI와 WMI 호출은 모두 그 스레드에서 하고, 슬라이더를 끄는 동안에는 모니터마다 마지막 값만 씁니다. 그래서 느린 모니터가 작업표시줄을 붙잡지 않습니다. 모니터 목록은 창을 열 때와 디스플레이가 바뀔 때 읽습니다.

## 필요한 것

- Windows 11, 64비트
- Bloom 0.19.0 이상
- 외부 모니터는 모니터 메뉴에서 DDC/CI가 켜져 있어야 합니다(대부분 처음부터 켜져 있음)
- 처음 시작할 때 인터넷 연결(아래 참고)

## 설치

1. [Releases](https://github.com/hyeon1035298/MonitorBrightness_BloomAddon/releases)에서 `MonitorBrightness_BloomAddon-<버전>.zip`을 받습니다.
2. `%LOCALAPPDATA%\Bloom\user-addons\brightness\`에 풉니다. 폴더 이름은 `brightness`여야 합니다.
3. Bloom을 다시 시작하고 설정 › 애드온에서 "모니터 밝기 조정"을 켭니다.

Bloom은 애드온을 시작하기 전에 모든 파일을 `addon.json`의 SHA-256과 대조합니다. 지우려면 애드온을 끄고 폴더를 지우면 됩니다.

### 처음 시작

모드는 `taskbar.dll`의 내부 함수를 거쳐 작업표시줄 XAML에 닿으므로 호스트에 그 DLL의 기호가 필요합니다. 처음 시작할 때와 Windows 업데이트로 DLL이 바뀐 뒤에 Microsoft 기호 서버에서 맞는 공개 기호 파일을 받아 오프셋만 남깁니다. Microsoft의 파일은 애드온에 들어 있지 않습니다.

## 쓰는 법

- **아이콘 위 휠**: 보이는 모든 모니터를 한 단계(처음 5 %)씩 올리거나 내립니다.
- **아이콘 누르기**: 창이 열립니다. 트랙을 끌거나 누르고, 줄 위에서 휠을 굴리고(한 칸 1 %), 숫자를 눌러 직접 입력합니다. 줄 끝의 화살표를 누르면 그 모니터의 대비와 [이 모니터 끄기]가 나옵니다. 그렇게 끈 모니터는 모니터의 전원 버튼으로 켭니다.
- **창 위쪽**: [모두 함께], 모든 화면 끄기(마우스나 키를 움직이면 켜짐), 설정.
- **설정**: 작업표시줄 숫자(첫 모니터·평균), 휠 한 칸, 모니터별 범위와 창에 보일지, 단축키(칸을 누르고 새 조합을 누름. Backspace는 지우기, Esc는 그대로), 한 번에 바뀌는 양, 유휴 시 어둡게.

DDC/CI가 꺼진 모니터는 창에 이름과 [다시 찾기] 버튼으로 알립니다. 다른 프로그램이 이미 쓰는 단축키는 그 칸에 적힙니다.

라이트·다크는 Bloom의 테마를, 창의 유리는 Bloom의 유리 효과를 따릅니다. 나머지 설정은 모드가 `%LOCALAPPDATA%\Bloom\addons\brightness\prefs.json`에 저장합니다.

## 소스에서 빌드

`x86_64-w64-mingw32`를 대상으로 하는 clang(llvm-mingw나 Windhawk에 딸린 컴파일러), Python 3, Git Bash 같은 bash 셸이 필요합니다. PillTaskbar의 애드온 도구로 빌드하므로 두 저장소를 나란히 받습니다.

```
git clone https://github.com/hyeon1035298/MonitorBrightness_BloomAddon
git clone https://github.com/hyeon1035298/PillTaskbar_BloomAddon
cd MonitorBrightness_BloomAddon
export CXX=/path/to/llvm-mingw/bin/clang++.exe   # 없으면 Windhawk의 컴파일러
bash build.sh
```

PillTaskbar_BloomAddon을 다른 곳에 받았다면 `PILL_ROOT`로 가리킵니다. llvm-mingw에는 C++/WinRT 헤더가 없으므로, 워크플로처럼 Windows SDK의 `cppwinrt.exe`가 만든 폴더를 `CPPWINRT_INCLUDE`로 지정합니다. 결과는 `build/addon`에 생깁니다.

`.github/workflows/build.yml`은 푸시마다 `PILL_REF`에 고정한 PillTaskbar_BloomAddon 커밋으로 빌드합니다. `v*` 태그는 zip, 그 SHA-256, 두 저장소의 커밋을 적은 `SOURCE.txt`를 릴리스로 올립니다.

## 라이선스

이 애드온은 GNU General Public License 버전 3(3 한정)의 자유 소프트웨어입니다. `LICENSE`를 보십시오.

런타임과 호스트에는 MinHook(BSD-2-Clause)과 Zydis(MIT)가 들어 있습니다. 출처와 라이선스 전문은 `THIRD_PARTY.md`와 릴리스 zip의 `THIRD_PARTY_NOTICES.txt`에 있습니다.

이 프로그램에는 아무 보증이 없습니다. explorer.exe 안에서 코드를 실행하고 모니터에 명령을 보내므로 각자 책임 아래 쓰십시오. Windows는 Microsoft의 상표입니다. 이 프로젝트는 Microsoft나 다른 밝기 도구의 제작자와 관계가 없습니다.

# ClickMaker

MTR·공연용 메트로놈 클릭과 큐 멘트("Pre-Chorus, 4, 3, 2, 1")를 만드는 Logic Pro용 AU 악기 플러그인입니다.
클릭은 Logic의 템포·박자표를 따라가고, 큐는 MIDI 노트 하나를 찍으면 macOS 음성(TTS)이 박에 맞춰 말합니다.

## 빌드와 설치

필요한 것: macOS 14 이상, Xcode, Homebrew, 인터넷(첫 빌드 때 JUCE를 내려받음)

```sh
brew install cmake ninja
git clone https://github.com/KilJH/clickmaker-vsti.git
cd clickmaker-vsti
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

빌드가 끝나면 `~/Library/Audio/Plug-Ins/Components/ClickMaker.component`에 자동으로 설치됩니다.

- Logic에 안 보이면 `killall -9 AudioComponentRegistrar`를 실행하고 Logic을 다시 열거나, Logic 설정 → 플러그인 관리자에서 다시 스캔하세요.
- 설치 확인: `auval -v aumu Clmk Ckmk`
- Intel Mac이면 configure 명령에 `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`를 추가하세요.

## Logic에서 쓰기

- 소프트웨어 악기 트랙 → AU Instruments → ClickMaker → ClickMaker
  - 클릭과 큐를 다른 채널로 받으려면 Multi Output으로 넣고 `큐 분리 출력`을 켭니다.
- 재생하면 클릭이 나옵니다. 쪼개기(8분·16분·셋잇단), 스윙, 강세, 음정·볼륨, 팬은 플러그인 창에서 조절합니다. 강세·박·쪼갬 볼륨을 맨 아래(Off)로 내리면 그 소리만 꺼집니다(예: 박을 꺼서 첫 박만 치기).
- 큐는 C1~B2 노트입니다. 큐가 시작될 마디의 첫 박보다 16분 앞에 짧은 노트를 찍으세요. 첫 단어는 자음 길이(0.05~0.12초)만큼 먼저 시작해야 모음이 박에 맞는데, 소리는 노트보다 먼저 날 수 없기 때문입니다.
  - 이름이 있는 슬롯은 기본으로 이름이 한 마디를 따로 차지해서 `| Pre-Chorus | 4 3 2 1 |`, 섹션 2마디 전에 찍습니다(8박이면 3마디 전).
  - `이름 길이`를 1박으로 하면 `| Chorus 3 2 1 |`, 2박으로 하면 `| Pre-Chorus · 2 1 |`처럼 이름이 카운트의 첫 박에 들어가 한 마디로 끝납니다. 전체 기본값은 큐 패널 위쪽에서, 슬롯별로는 목록의 `이름 길이`에서 정합니다. 템포가 빠른데 이름이 길면 2박을 쓰세요.
  - 각 슬롯의 '노트 위치'에 섹션 몇 마디 전에 찍을지 나옵니다.
  - 큐는 항상 마디 첫 박에서 시작합니다. 노트 다음의 마디선에서 시작하고, 첫 박보다 16분 이내로 늦은 노트는 그 마디에서 시작합니다. 첫 박에 딱 맞춰 찍어도 되지만 첫 단어가 자음 길이만큼 늦게 들립니다.
  - 큐 도중 박자표가 바뀌면 그 마디의 박 수로 셉니다. 섹션 앞 마디가 2/4면 `| Chorus | 2 1 |`이 됩니다. Logic이 새 박자표를 그 마디가 시작될 때 알려주므로, 하행 카운트는 그 마디의 첫 숫자가 조금 늦게 들립니다.
- 트랙에는 곡 전체를 덮는 MIDI 리전을 1개 두세요. Logic은 리전이 없는 트랙을 처리하지 않을 수 있습니다.
- 정지 중 미리듣기(▶ 버튼, 건반)는 트랙이 선택된 상태에서만 들립니다.
- 재생 중에 건반·패드·Logic Remote로 큐 노트를 직접 눌러도 됩니다. 마디 중간에 누르면 다음 마디부터 나오므로, 섹션의 '노트 위치'만큼 앞 마디에서 누르세요. Logic은 선택했거나 녹음 대기인 트랙에만 건반 입력을 보냅니다.
- 마커로 곡 중간에 점프해 재생한다면 마커를 섹션 첫 박이 아니라 그 섹션의 큐 노트보다 앞에 두세요. 큐 노트 뒤에서 재생을 시작하면 그 큐는 나오지 않습니다.
- 만든 음성은 프로젝트에 함께 저장되어 다른 Mac에서도 같은 소리가 납니다.
- 큐 이름·음성·클릭 설정을 다른 곡에도 쓰려면 플러그인 창 위쪽 설정 메뉴에서 설정을 저장하거나 기본값으로 저장하세요(Save Setting As, Save as Default). 만든 음성도 함께 저장됩니다.

## 테스트

```sh
ctest --test-dir build --output-on-failure
build/ClickMakerTests_artefacts/Release/ClickMakerTests --tts-smoke   # 실제 음성으로 렌더 → build/test-output/*.wav
```

## 배포 (설치 파일, GitHub Release)

`v1.2.3` 같은 태그를 푸시하면 GitHub Actions가 다음을 합니다.

1. Apple Silicon·Intel 겸용으로 빌드
2. 테스트
3. 설치 프로그램(`.pkg`), zip, SHA-256 목록을 GitHub Release에 올림

태그의 숫자가 플러그인 버전이 됩니다. Logic은 이 버전이 바뀌면 플러그인을 다시 검사합니다.

```sh
git tag v0.1.0
git push origin v0.1.0
```

- Actions 탭 → Release → Run workflow로 실행하면 릴리스는 만들지 않고, 같은 파일을 워크플로 아티팩트로 받을 수 있습니다.
- 로컬에서 설치 파일 만들기: 빌드한 뒤 `scripts/package-macos.sh 0.1.0`을 실행하면 `dist/`에 생깁니다.

### 서명과 공증 (선택)

아래 시크릿이 없으면 ad-hoc 서명으로 배포되고, 받는 사람이 처음 한 번 Gatekeeper 경고를 넘겨야 합니다(릴리스 노트에 방법이 자동으로 들어감).
Apple Developer Program(연 $99)에 가입한 뒤 시크릿을 넣으면, 다음 릴리스부터 Developer ID 서명과 공증이 자동으로 됩니다.

| 시크릿 | 내용 |
|---|---|
| `MACOS_APPLICATION_P12` | Developer ID Application 인증서 .p12 (base64) |
| `MACOS_INSTALLER_P12` | Developer ID Installer 인증서 .p12 (base64) |
| `MACOS_P12_PASSWORD` | 두 .p12를 내보낼 때 쓴 비밀번호 |
| `NOTARY_API_KEY` | App Store Connect API 키 파일(.p8) 내용 |
| `NOTARY_API_KEY_ID` | 그 키의 Key ID |
| `NOTARY_API_ISSUER` | Issuer ID |

```sh
gh secret set MACOS_APPLICATION_P12 < <(base64 -i DeveloperIDApplication.p12)
gh secret set MACOS_INSTALLER_P12 < <(base64 -i DeveloperIDInstaller.p12)
gh secret set MACOS_P12_PASSWORD
gh secret set NOTARY_API_KEY < AuthKey_XXXXXXXXXX.p8
gh secret set NOTARY_API_KEY_ID
gh secret set NOTARY_API_ISSUER
```

- **인증서:** Xcode → 설정 → Accounts → Manage Certificates에서 Developer ID Application과 Developer ID Installer를 만든 뒤, 키체인 접근에서 각각 .p12로 내보냅니다.
- **API 키:** App Store Connect → 사용자 및 액세스 → 통합 → App Store Connect API에서 팀 키를 만듭니다(Developer 역할 이상).

빌드한 바이너리를 공개 배포하기 전에 JUCE 라이선스를 정해야 합니다. 이 저장소를 AGPLv3로 공개(LICENSE 파일 추가)하거나, JUCE Starter 라이선스(연 매출 $20k 이하 무료)를 쓰세요.

## Logic에서 확인할 것

1. 트랙을 선택했을 때와 안 했을 때 모두 클릭이 끊기지 않는지, 박자표 4/4 → 6/8 → 7/8 변화에서 강세가 맞는지
2. 1마디부터 재생할 때 첫 카운트가 나오는지, 사이클(루프) 경계에서 클릭이 겹치지 않는지
3. 플러그인 창에서 한글 입력이 되는지 (Musical Typing이 켜져 있으면 키 입력을 가져갈 수 있음)
4. Multi Output으로 넣었을 때 `큐 분리 출력`을 켜면 큐가 Aux로 나오는지
5. 저장 후 다시 열었을 때, Bounce in Place로 내보냈을 때 큐가 바로 정상으로 나오는지
6. 섹션 앞에 2/4 마디가 있을 때 카운트가 `2 1`로 줄어 섹션 첫 박 전에 끝나는지
7. 사이클 시작점에 찍은 큐가 반복할 때마다 나오는지 (Logic이 사이클 끝에서 처리 블록을 나누지 않으면 빠질 수 있음)
8. 이름·숫자 칸에 입력하고 Return을 누른 뒤 Space로 Logic 재생·정지가 되는지
9. 토글·콤보·▶·슬라이더를 클릭한 뒤 Return(처음으로)·↑↓(트랙 선택)·Space가 Logic으로 가고 플러그인 값은 그대로인지
10. 재생 중 트랙을 선택하고 마디 중간에 큐 건반을 누르면 다음 마디 첫 박부터 카운트가 나오는지
11. 강세·박·쪼갬 볼륨을 Off로 두면 그 소리만 꺼지고, Logic 오토메이션에도 Off로 보이는지

JUCE는 빌드할 때 내려받으며 JUCE 라이선스(AGPLv3 또는 JUCE EULA)를 따릅니다.

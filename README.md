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
- 재생하면 클릭이 나옵니다. 쪼개기(8분·16분·셋잇단), 스윙, 강세, 음정·볼륨, 팬은 플러그인 창에서 조절합니다.
- 큐는 C1~B2 노트입니다. 큐가 시작될 마디의 첫 박에 짧은 노트(16분)를 찍으세요.
  - 이름이 있는 슬롯은 `| Pre-Chorus | 4 3 2 1 |` 순서라서 섹션 2마디 전에 찍습니다(8박이면 3마디 전).
  - 각 슬롯의 '노트 위치'에 섹션 몇 마디 전에 찍을지 나옵니다.
- 트랙에는 곡 전체를 덮는 MIDI 리전을 1개 두세요. Logic은 리전이 없는 트랙을 처리하지 않을 수 있습니다.
- 정지 중 미리듣기(▶ 버튼, 건반)는 트랙이 선택된 상태에서만 들립니다.
- 만든 음성은 프로젝트에 함께 저장되어 다른 Mac에서도 같은 소리가 납니다.

## 테스트

```sh
ctest --test-dir build --output-on-failure
build/ClickMakerTests_artefacts/Release/ClickMakerTests --tts-smoke   # 실제 음성으로 렌더 → build/test-output/*.wav
```

## Logic에서 확인할 것

1. 트랙을 선택했을 때와 안 했을 때 모두 클릭이 끊기지 않는지, 박자표 4/4 → 6/8 → 7/8 변화에서 강세가 맞는지
2. 1마디부터 재생할 때 첫 카운트가 나오는지, 사이클(루프) 경계에서 클릭이 겹치지 않는지
3. 플러그인 창에서 한글 입력이 되는지 (Musical Typing이 켜져 있으면 키 입력을 가져갈 수 있음)
4. Multi Output으로 넣었을 때 `큐 분리 출력`을 켜면 큐가 Aux로 나오는지
5. 저장 후 다시 열었을 때, Bounce in Place로 내보냈을 때 큐가 바로 정상으로 나오는지

JUCE는 빌드할 때 내려받으며 JUCE 라이선스(AGPLv3 또는 JUCE EULA)를 따릅니다.

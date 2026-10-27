# Copilot Instructions

## Language

- Always respond in the language the user chats with you.
- Write all AI-generated repository content in English, including source code comments, documentation, tests, commit messages, issue or pull request text, user-facing strings, and configuration descriptions.
- Preserve established technical names, identifiers, product names, and protocol terms where appropriate; do not translate proper nouns.
- If the user explicitly asks for a translation into another language, provide that translation only as the requested deliverable. Keep other project content in English.

## Local ESP-IDF Builds

- Local builds run in GitHub Codespaces, where CPU and memory are shared and may be limited. Avoid starting expensive firmware builds unless they are relevant to the change, and do not repeat a build that already passed without a code or configuration change.
- Keep the local `build/` directory between builds. It is ignored by Git and contains the CMake/Ninja state and compiled artifacts needed for incremental builds.
- For firmware changes, run `idf.py build` from the repository root once when a build is needed. Do not delete `build/`, run `idf.py fullclean`, or rerun `idf.py set-target esp32s3` unless the target or build configuration has changed or the existing build state is demonstrably stale.
- Do not run a firmware build for documentation-only or web-only changes.
- If `ccache` is installed and available in the ESP-IDF environment, use `idf.py --ccache build` to accelerate recompilation across build directories. Do not install or configure tools as part of a routine build.

## Host Tests

- Hardware-independent logic (packet parsing/building, BLE frame reassembly, hex parsing) lives in files without ESP-IDF includes and is tested in `test/host`.
- Run host tests with `cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure`. They are fast and do not need ESP-IDF; prefer them over firmware builds when only such logic changed.

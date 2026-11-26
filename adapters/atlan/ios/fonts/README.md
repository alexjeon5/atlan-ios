<!-- SPDX-License-Identifier: OFL-1.1 -->
# Launcher fonts

The launcher design uses Barlow for Latin text and numbers and Noto Sans KR for Korean. Both are under the SIL Open
Font License 1.1 (`OFL-Barlow.txt`, `OFL-NotoSansKR.txt`) and come from the Google Fonts repository
(github.com/google/fonts, `ofl/barlow`, `ofl/notosanskr`).

- `Barlow-Regular.ttf`, `Barlow-Medium.ttf`, `Barlow-SemiBold.ttf`: unmodified.
- `NotoSansKR-Hangul.ttf`: `NotoSansKR[wght].ttf` (variable weight axis kept) subset with fontTools `pyftsubset` to
  Latin, Hangul (syllables, jamo, compatibility jamo), general punctuation, arrows, enclosed numbers, shapes, CJK
  punctuation and full-width forms, which brings it from 10 MB to 5 MB. Other characters fall back to the system font.

```
pyftsubset 'NotoSansKR[wght].ttf' --layout-features='*' --output-file=NotoSansKR-Hangul.ttf \
  --unicodes='U+0020-007E,U+00A0-00FF,U+1100-11FF,U+2000-206F,U+2190-21FF,U+2460-24FF,U+25A0-25FF,U+3000-303F,U+3130-318F,U+AC00-D7A3,U+FF00-FFEF'
```

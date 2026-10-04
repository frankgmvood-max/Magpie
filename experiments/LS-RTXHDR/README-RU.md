# RTX Video HDR для LS 0.1.0

NVIDIA TrueHDR backend перенесён из SAOG0721/Magpie 0.6.9,
commit 2fceab5e241bc9f8ded001ab3266762f1f8bc51e. RtxVideoBridge.cpp/.h скопированы без изменений и проверены по текущему upstream.
Это SDK-преобразование SDR -> scRGB FP16. Официальный NVIDIA runtime и лицензия включены.

Отключите HDR LS и другие SDR/HDR преобразователи. В меню аддона включите RTX HDR,
подтвердите отключение HDR LS и сохраните. Вход SDR RGBA8/BGRA8; уже HDR/FP16 вход не преобразуется повторно.
Параметры как в Magpie: Contrast 0–200 (100), Saturation 0–200 (100), Middle gray 10–100 (50),
Peak brightness 400–2000 нит (1000). Runtime RTX Video SDK 1.1 изолирован от DLSS loader.
Не заменяйте nvngx_truehdr.dll DLL мода DLSS FG для RTX 30.

Auto выбирает scRGB HDR, когда HDR Windows активен на текущем мониторе; иначе адаптирует результат к SDR.
SDR display mode использует luminance shoulder и sRGB encode из Magpie HdrSurfaceAdapter, с белым, экспозицией и плечом.
Это отображение HDR-результата на SDR, а не аппаратный HDR монитора. HDR scRGB сохраняет FP16 и требует HDR Windows.
Системные настройки дисплеев аддон не изменяет.

Оригинальные и сгенерированные DLSS FG кадры обрабатываются через общий мост. Smooth Motion может использовать FP16 напрямую.
Вычисления идут на отдельном D3D11 контексте того же адаптера. После смены устройства/формата/размера или сворачивания ресурсы обновляются.
Ошибки NVIDIA сохраняют вывод LS и записывают HRESULT/NGX status. В меню/Performance видны время, число обработанных кадров и повторы.

Код Magpie и производный адаптер распространяются с исходниками и GPL v3. Лицензия SDK NVIDIA включена отдельным PDF.
CI проверяет GPU transport на WARP с тестовым TrueHDR provider; качество и совместимость NVIDIA на RTX 3080 требует аппаратной проверки.

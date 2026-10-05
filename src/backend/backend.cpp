// moc's metatype for Backend::replied copies the nested Result variant. At -O3,
// GCC's -Wmaybe-uninitialized reports false positives inside that inlined
// std::variant/std::optional copy (no member is read unset). The diagnostic
// is attributed to where the metatype templates are first seen, so this TU
// holds only the generated code and silences it from the top; every other
// user of types.h keeps the warning.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "moc_backend.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

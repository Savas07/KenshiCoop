// Compatibility shim for KenshiLib >= 0.4.0, which moved CombatClass.h into
// kenshi/combat/. The plugin includes it by its old path; this keeps the source
// building unchanged against the KenshiLib that RE_Kenshi 0.3.5 ships (0.5.0).
#pragma once
#include <kenshi/combat/CombatClass.h>

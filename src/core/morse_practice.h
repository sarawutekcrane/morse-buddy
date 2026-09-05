#pragma once
// Morse Practice full mode (Addendum section 20; Phase 3 sections 16-17).
// Entirely self-registering: a static registrar in morse_practice.cpp calls
// Settings::registerMorsePracticeStartHandler(...) and registerSettingItem
// (Audio Preview, Reveal Answer) into Settings::kMorsePracticeListId — no
// other file needs to change. The Level 2/50-word and Level 3/20-sentence
// datasets are hard-coded in morse_practice.cpp per the Addendum's "generate
// once, hard-code, never re-randomize" instruction.

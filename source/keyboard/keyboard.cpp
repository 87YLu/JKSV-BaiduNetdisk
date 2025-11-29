#include "keyboard/keyboard.hpp"

#include "error.hpp"

#include <string>

bool keyboard::get_input(SwkbdType keyboardType,
                         std::string_view defaultText,
                         std::string_view header,
                         char *stringOut,
                         size_t stringLength,
                         std::optional<std::reference_wrapper<keyboard::Dictionary>> dictionary)
{
    // Swkbd config.
    SwkbdConfig keyboard{};

    // Cache instead of repeated calls.
    const bool hasDictionary = dictionary.has_value();
    const size_t wordCount   = hasDictionary ? dictionary->get().get_count() : 0;

    // If this fails, immediately return false.
    const bool createError = error::libnx(swkbdCreate(&keyboard, wordCount));
    if (createError) { return false; }

    // Standard swkbd init.
    swkbdConfigSetBlurBackground(&keyboard, true);
    swkbdConfigSetInitialText(&keyboard, defaultText.data());
    swkbdConfigSetHeaderText(&keyboard, header.data());
    swkbdConfigSetGuideText(&keyboard, header.data());
    swkbdConfigSetType(&keyboard, keyboardType);
    swkbdConfigSetStringLenMax(&keyboard, stringLength);
    swkbdConfigSetKeySetDisableBitmask(&keyboard, SwkbdKeyDisableBitmask_Backslash);

    // Add the dictionary if one was passed.
    if (hasDictionary)
    {
        // These are easier to read and work with.
        const SwkbdDictWord *words = reinterpret_cast<const SwkbdDictWord *>(dictionary->get().get_words());
        const size_t wordCount     = dictionary->get().get_count();

        // Set the flags.
        swkbdConfigSetDicFlag(&keyboard, 1);
        // Add our word dictionary.
        swkbdConfigSetDictionary(&keyboard, words, wordCount);
    }

    const bool swkbdError = error::libnx(swkbdShow(&keyboard, stringOut, stringLength));
    const bool empty      = std::char_traits<char>::length(stringOut) == 0;
    if (swkbdError || empty)
    {
        swkbdClose(&keyboard);
        return false;
    }

    swkbdClose(&keyboard);
    return true;
}

#include "keyboard/Dictionary.hpp"

#include "logging/logger.hpp"

//                      ---- Construction ----

keyboard::Dictionary::Dictionary(std::initializer_list<std::string_view> wordList) { Dictionary::add_list_to_list(wordList); }

//                      ---- Public functions ----

void keyboard::Dictionary::add_word_to_list(std::string_view word)
{
    // New word.
    Dictionary::Word newWord{};

    // LibNX's functions for this expect uint16 instead of char16... If there's even much of a difference?
    uint16_t *predict  = reinterpret_cast<uint16_t *>(newWord.predict);
    uint16_t *dictWord = reinterpret_cast<uint16_t *>(newWord.word);

    // This makes the following easier to read and less repetitive.
    const uint8_t *inData = reinterpret_cast<const uint8_t *>(word.data());

    // The dictionary suggestions are UTF-16.
    utf8_to_utf16(predict, inData, Dictionary::WORD_LENGTH);
    utf8_to_utf16(dictWord, inData, Dictionary::WORD_LENGTH);

    m_words.push_back(newWord);
}

void keyboard::Dictionary::add_list_to_list(std::initializer_list<std::string_view> wordList)
{
    // Loop through the list.
    for (const std::string_view word : wordList) { Dictionary::add_word_to_list(word); }
}

size_t keyboard::Dictionary::get_count() const noexcept { return m_words.size(); }

const keyboard::Dictionary::Word *keyboard::Dictionary::get_words() const noexcept { return m_words.data(); }

//                      ---- Private functions ----

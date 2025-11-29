#include "keyboard/Dictionary.hpp"

#include "logging/logger.hpp"

//                      ---- Construction ----

keyboard::Dictionary::Dictionary(std::initializer_list<std::string_view> wordList) { Dictionary::add_list(wordList); }

//                      ---- Public functions ----

void keyboard::Dictionary::add_list(std::initializer_list<std::string_view> wordList)
{
    // Loop through the list.
    for (const std::string_view word : wordList)
    {
        // New word
        Dictionary::Word newWord{};

        // This is cleaner and easier to read. libnx expects uint16 instead of char16
        uint16_t *predict  = reinterpret_cast<uint16_t *>(newWord.predict);
        uint16_t *dictWord = reinterpret_cast<uint16_t *>(newWord.word);

        // Same here.
        const uint8_t *inData = reinterpret_cast<const uint8_t *>(word.data());

        // The words are UTF-16. We need to convert them.
        utf8_to_utf16(predict, inData, Dictionary::WORD_LENGTH);
        utf8_to_utf16(dictWord, inData, Dictionary::WORD_LENGTH);

        // Push the word to the vector.
        m_words.push_back(newWord);
    }
}

size_t keyboard::Dictionary::get_count() const noexcept { return m_words.size(); }

const keyboard::Dictionary::Word *keyboard::Dictionary::get_words() const noexcept { return m_words.data(); }
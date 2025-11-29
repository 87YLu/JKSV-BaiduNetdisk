#pragma once
#include <initializer_list>
#include <span>
#include <string>
#include <switch.h>
#include <vector>

namespace keyboard
{
    class Dictionary final
    {
        public:
            /// @brief Length of the words.
            static constexpr size_t WORD_LENGTH = 0x19;

            // clang-format off
            struct Word
            {
                /// @brief Need to confirm, but I'm pretty sure this is what the system uses to predict?
                char16_t predict[WORD_LENGTH]{};

                /// @brief This is the actual word.
                char16_t word[WORD_LENGTH]{};
            };
            // clang-format on

            /// @brief Default constructor.
            Dictionary() = default;

            /// @brief Constructs a new dictionary using the list passed.
            Dictionary(std::initializer_list<std::string_view> wordList);

            /// @brief Helper function to add word to the internal list to cut down on repetition.
            void add_word_to_list(std::string_view word);

            /// @brief Adds the list to the internal list.
            void add_list_to_list(std::initializer_list<std::string_view> wordList);

            /// @brief Returns the number of words in the internal list.
            size_t get_count() const noexcept;

            /// @brief Returns the pointer to the internal word vector data.
            const Dictionary::Word *get_words() const noexcept;

        private:
            /// @brief Vector of words.
            std::vector<Dictionary::Word> m_words{};
    };
}
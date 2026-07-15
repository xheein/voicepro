// language.h
#pragma once
#include <string>
#include <unordered_map>

enum class Language
{
    Chinese,
    English
};

class LanguageManager
{
public:
    static LanguageManager& Instance();

    void SetLanguage(Language lang);
    Language GetLanguage() const;

    const std::wstring& Get(const std::wstring& key) const;

private:
    LanguageManager();
    ~LanguageManager() = default;
    LanguageManager(const LanguageManager&) = delete;
    LanguageManager& operator=(const LanguageManager&) = delete;

    Language current_;

    std::unordered_map<std::wstring, std::wstring> zh_;
    std::unordered_map<std::wstring, std::wstring> en_;
};
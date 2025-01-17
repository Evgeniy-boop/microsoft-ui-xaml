﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"
#include "common.h"
#include "NumberBox.h"
#include "NumberBoxAutomationPeer.h"
#include "NumberBoxParser.h"
#include "RuntimeProfiler.h"
#include "ResourceAccessor.h"
#include "Utils.h"

static constexpr wstring_view c_numberBoxDownButtonName{ L"DownSpinButton"sv };
static constexpr wstring_view c_numberBoxUpButtonName{ L"UpSpinButton"sv };
static constexpr wstring_view c_numberBoxTextBoxName{ L"InputBox"sv };
static constexpr wstring_view c_numberBoxPopupButtonName{ L"PopupButton"sv };
static constexpr wstring_view c_numberBoxPopupName{ L"UpDownPopup"sv };
static constexpr wstring_view c_numberBoxPopupDownButtonName{ L"PopupDownSpinButton"sv };
static constexpr wstring_view c_numberBoxPopupUpButtonName{ L"PopupUpSpinButton"sv };
static constexpr wstring_view c_numberBoxPopupContentRootName{ L"PopupContentRoot"sv };

static constexpr double c_popupShadowDepth = 16.0;
static constexpr wstring_view c_numberBoxPopupShadowDepthName{ L"NumberBoxPopupShadowDepth"sv };

// Shockingly, there is no standard function for trimming strings.
const std::wstring c_whitespace = L" \n\r\t\f\v";
std::wstring trim(const std::wstring& s)
{
    size_t start = s.find_first_not_of(c_whitespace);
    size_t end = s.find_last_not_of(c_whitespace);
    return (start == std::wstring::npos || end == std::wstring::npos) ? L"" : s.substr(start, end - start + 1);
}

NumberBox::NumberBox()
{
    __RP_Marker_ClassById(RuntimeProfiler::ProfId_NumberBox);

    // Default values for the number formatter
    const auto formatter = winrt::DecimalFormatter();
    formatter.IntegerDigits(1);
    formatter.FractionDigits(0);
    NumberFormatter(formatter);

    PointerWheelChanged({ this, &NumberBox::OnNumberBoxScroll });

    GotFocus({ this, &NumberBox::OnNumberBoxGotFocus });
    LostFocus({ this, &NumberBox::OnNumberBoxLostFocus });

    SetDefaultStyleKey(this);
}

winrt::AutomationPeer NumberBox::OnCreateAutomationPeer()
{
    return winrt::make<NumberBoxAutomationPeer>(*this);
}

void NumberBox::OnApplyTemplate()
{
    const winrt::IControlProtected controlProtected = *this;

    const auto spinDownName = ResourceAccessor::GetLocalizedStringResource(SR_NumberBoxDownSpinButtonName);
    const auto spinUpName = ResourceAccessor::GetLocalizedStringResource(SR_NumberBoxUpSpinButtonName);

    if (const auto spinDown = GetTemplateChildT<winrt::RepeatButton>(c_numberBoxDownButtonName, controlProtected))
    {
        m_downButtonClickRevoker = spinDown.Click(winrt::auto_revoke, { this, &NumberBox::OnSpinDownClick });

        // Do localization for the down button
        if (winrt::AutomationProperties::GetName(spinDown).empty())
        {
            winrt::AutomationProperties::SetName(spinDown, spinDownName);
        }
    }

    if (const auto spinUp = GetTemplateChildT<winrt::RepeatButton>(c_numberBoxUpButtonName, controlProtected))
    {
        m_upButtonClickRevoker = spinUp.Click(winrt::auto_revoke, { this, &NumberBox::OnSpinUpClick });

        // Do localization for the up button
        if (winrt::AutomationProperties::GetName(spinUp).empty())
        {
            winrt::AutomationProperties::SetName(spinUp, spinUpName);
        }
    }

    m_textBox.set([this, controlProtected]() {
        const auto textBox = GetTemplateChildT<winrt::TextBox>(c_numberBoxTextBoxName, controlProtected);
        if (textBox)
        {
            m_textBoxKeyDownRevoker = textBox.KeyDown(winrt::auto_revoke, { this, &NumberBox::OnNumberBoxKeyDown });
            m_textBoxKeyUpRevoker = textBox.KeyUp(winrt::auto_revoke, { this, &NumberBox::OnNumberBoxKeyUp });
        }
        return textBox;
    }());

    m_popup.set(GetTemplateChildT<winrt::Popup>(c_numberBoxPopupName, controlProtected));

    if (SharedHelpers::IsThemeShadowAvailable())
    {
        if (const auto popupRoot = GetTemplateChildT<winrt::UIElement>(c_numberBoxPopupContentRootName, controlProtected))
        {
            if (!popupRoot.Shadow())
            {
                popupRoot.Shadow(winrt::ThemeShadow{});
                auto&& translation = popupRoot.Translation();

                const double shadowDepth = unbox_value<double>(SharedHelpers::FindResource(c_numberBoxPopupShadowDepthName, winrt::Application::Current().Resources(), box_value(c_popupShadowDepth)));

                popupRoot.Translation({ translation.x, translation.y, (float)shadowDepth });
            }
        }
    }

    if (const auto popupSpinDown = GetTemplateChildT<winrt::RepeatButton>(c_numberBoxPopupDownButtonName, controlProtected))
    {
        m_popupDownButtonClickRevoker = popupSpinDown.Click(winrt::auto_revoke, { this, &NumberBox::OnSpinDownClick });
    }

    if (const auto popupSpinUp = GetTemplateChildT<winrt::RepeatButton>(c_numberBoxPopupUpButtonName, controlProtected))
    {
        m_popupUpButtonClickRevoker = popupSpinUp.Click(winrt::auto_revoke, { this, &NumberBox::OnSpinUpClick });
    }

    // .NET rounds to 12 significant digits when displaying doubles, so we will do the same.
    m_displayRounder.SignificantDigits(12);

    UpdateSpinButtonPlacement();
    UpdateSpinButtonEnabled();

    if (ReadLocalValue(s_ValueProperty) == winrt::DependencyProperty::UnsetValue()
        && ReadLocalValue(s_TextProperty) != winrt::DependencyProperty::UnsetValue())
    {
        // If Text has been set, but Value hasn't, update Value based on Text.
        UpdateValueToText();
    }
    else
    {
        UpdateTextToValue();
    }
}

void NumberBox::OnValuePropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    // This handler may change Value; don't send extra events in that case.
    if (!m_valueUpdating)
    {
        const auto oldValue = unbox_value<double>(args.OldValue());

        auto scopeGuard = gsl::finally([this]()
        {
            m_valueUpdating = false;
        });
        m_valueUpdating = true;

        CoerceValue();

        const auto newValue = Value();
        if (newValue != oldValue && !(std::isnan(newValue) && std::isnan(oldValue)))
        {
            // Fire ValueChanged event
            const auto valueChangedArgs = winrt::make_self<NumberBoxValueChangedEventArgs>(oldValue, newValue);
            m_valueChangedEventSource(*this, *valueChangedArgs);

            // Fire value property change for UIA
            if (const auto peer = winrt::FrameworkElementAutomationPeer::FromElement(*this).as<winrt::NumberBoxAutomationPeer>())
            {
                winrt::get_self<NumberBoxAutomationPeer>(peer)->RaiseValueChangedEvent(oldValue, newValue);
            }
        }

        UpdateTextToValue();
        UpdateSpinButtonEnabled();
    }
}

void NumberBox::OnMinimumPropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    CoerceMaximum();
    CoerceValue();

    UpdateSpinButtonEnabled();
}

void NumberBox::OnMaximumPropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    CoerceMinimum();
    CoerceValue();

    UpdateSpinButtonEnabled();
}

void NumberBox::OnSmallChangePropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    UpdateSpinButtonEnabled();
}

void NumberBox::OnIsWrapEnabledPropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    UpdateSpinButtonEnabled();
}

void NumberBox::OnNumberFormatterPropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    // Update text with new formatting
    UpdateTextToValue();
}

void NumberBox::ValidateNumberFormatter(winrt::INumberFormatter2 value)
{
    // NumberFormatter also needs to be an INumberParser
    if (!value.try_as<winrt::INumberParser>())
    {
        throw winrt::hresult_error(E_INVALIDARG);
    }
}

void NumberBox::OnSpinButtonPlacementModePropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    UpdateSpinButtonPlacement();
}

void NumberBox::OnTextPropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    if (!m_textUpdating)
    {
        UpdateValueToText();
    }
}

void NumberBox::UpdateValueToText()
{
    if (auto && textBox = m_textBox.get())
    {
        textBox.Text(Text());
        ValidateInput();
    }
}

void NumberBox::OnValidationModePropertyChanged(const winrt::DependencyPropertyChangedEventArgs& args)
{
    ValidateInput();
    UpdateSpinButtonEnabled();
}

void NumberBox::OnNumberBoxGotFocus(winrt::IInspectable const& sender, winrt::RoutedEventArgs const& args)
{
    // When the control receives focus, select the text
    if (auto && textBox = m_textBox.get())
    {
        textBox.SelectAll();
    }

    if (SpinButtonPlacementMode() == winrt::NumberBoxSpinButtonPlacementMode::Compact)
    {
        if (auto && popup = m_popup.get())
        {
            popup.IsOpen(true);
        }
    }
}

void NumberBox::OnNumberBoxLostFocus(winrt::IInspectable const& sender, winrt::RoutedEventArgs const& args)
{
    ValidateInput();

    if (auto && popup = m_popup.get())
    {
        popup.IsOpen(false);
    }
}

void NumberBox::CoerceMinimum()
{
    const auto max = Maximum();
    if (Minimum() > max)
    {
        Minimum(max);
    }
}

void NumberBox::CoerceMaximum()
{
    const auto min = Minimum();
    if (Maximum() < min)
    {
        Maximum(min);
    }
}

void NumberBox::CoerceValue()
{
    // Validate that the value is in bounds
    const auto value = Value();
    if (!std::isnan(value) && !IsInBounds(value) && ValidationMode() == winrt::NumberBoxValidationMode::InvalidInputOverwritten)
    {
        // Coerce value to be within range
        const auto max = Maximum();
        if (value > max)
        {
            Value(max);
        }
        else
        {
            Value(Minimum());
        }
    }
}

void NumberBox::ValidateInput()
{
    // Validate the content of the inner textbox
    if (auto&& textBox = m_textBox.get())
    {
        const auto text = trim(textBox.Text().data());

        // Handles empty TextBox case, set text to current value
        if (text.empty())
        {
            Value(std::numeric_limits<double>::quiet_NaN());
        }
        else
        {
            // Setting NumberFormatter to something that isn't an INumberParser will throw an exception, so this should be safe
            const auto numberParser = NumberFormatter().as<winrt::INumberParser>();

            const winrt::IReference<double> value = AcceptsExpression()
                ? NumberBoxParser::Compute(text, numberParser)
                : numberParser.ParseDouble(text);

            if (!value)
            {
                if (ValidationMode() == winrt::NumberBoxValidationMode::InvalidInputOverwritten)
                {
                    // Override text to current value
                    UpdateTextToValue();
                }
            }
            else
            {
                if (value.Value() == Value())
                {
                    // Even if the value hasn't changed, we still want to update the text (e.g. Value is 3, user types 1 + 2, we want to replace the text with 3)
                    UpdateTextToValue();
                }
                else
                {
                    Value(value.Value());
                }
            }
        }
    }
}

void NumberBox::OnSpinDownClick(winrt::IInspectable const&  sender, winrt::RoutedEventArgs const& args)
{
    StepValue(-SmallChange());
}

void NumberBox::OnSpinUpClick(winrt::IInspectable const& sender, winrt::RoutedEventArgs const& args)
{
    StepValue(SmallChange());
}

void NumberBox::OnNumberBoxKeyDown(winrt::IInspectable const& sender, winrt::KeyRoutedEventArgs const& args)
{
    // Handle these on key down so that we get repeat behavior.
    switch (args.OriginalKey())
    {
        case winrt::VirtualKey::Up:
            StepValue(SmallChange());
            args.Handled(true); 
            break;

        case winrt::VirtualKey::Down:
            StepValue(-SmallChange());
            args.Handled(true);
            break;

        case winrt::VirtualKey::PageUp:
            StepValue(LargeChange());
            args.Handled(true);
            break;

        case winrt::VirtualKey::PageDown:
            StepValue(-LargeChange());
            args.Handled(true);
            break;
    }
}

void NumberBox::OnNumberBoxKeyUp(winrt::IInspectable const& sender, winrt::KeyRoutedEventArgs const& args)
{
    switch (args.OriginalKey())
    {
    case winrt::VirtualKey::Enter:
    case winrt::VirtualKey::GamepadA:
        ValidateInput();
        args.Handled(true);
        break;

    case winrt::VirtualKey::Escape:
    case winrt::VirtualKey::GamepadB:
        UpdateTextToValue();
        args.Handled(true);
        break;
    }
}

void NumberBox::OnNumberBoxScroll(winrt::IInspectable const& sender, winrt::PointerRoutedEventArgs const& args)
{
    if (auto && textBox = m_textBox.get())
    {
        if (textBox.FocusState() != winrt::FocusState::Unfocused)
        {
            const auto delta = args.GetCurrentPoint(*this).Properties().MouseWheelDelta();
            if (delta > 0)
            {
                StepValue(SmallChange());
            }
            else if (delta < 0)
            {
                StepValue(-SmallChange());
            }
        }
    }
}

void NumberBox::StepValue(double change)
{
    // Before adjusting the value, validate the contents of the textbox so we don't override it.
    ValidateInput();

    auto newVal = Value();
    if (!std::isnan(newVal))
    {
        newVal += change;

        if (IsWrapEnabled())
        {
            const auto max = Maximum();
            const auto min = Minimum();

            if (newVal > max)
            {
                newVal = min;
            }
            else if (newVal < min)
            {
                newVal = max;
            }
        }

        Value(newVal);
    }
}

// Updates TextBox.Text with the formatted Value
void NumberBox::UpdateTextToValue()
{
    if (auto && textBox = m_textBox.get())
    {
        winrt::hstring newText = L"";

        const auto value = Value();
        if (!std::isnan(value))
        {
            // Rounding the value here will prevent displaying digits caused by floating point imprecision.
            const auto roundedValue = m_displayRounder.RoundDouble(value);
            newText = NumberFormatter().FormatDouble(roundedValue);
        }

        textBox.Text(newText);

        auto scopeGuard = gsl::finally([this]()
        {
            m_textUpdating = false;
        });
        m_textUpdating = true;
        Text(newText.data());

        // This places the caret at the end of the text.
        textBox.Select(static_cast<int32_t>(newText.size()), 0);
    }
}

void NumberBox::UpdateSpinButtonPlacement()
{
    const auto spinButtonMode = SpinButtonPlacementMode();

    if (spinButtonMode == winrt::NumberBoxSpinButtonPlacementMode::Inline)
    {
        winrt::VisualStateManager::GoToState(*this, L"SpinButtonsVisible", false);
    }
    else if (spinButtonMode == winrt::NumberBoxSpinButtonPlacementMode::Compact)
    {
        winrt::VisualStateManager::GoToState(*this, L"SpinButtonsPopup", false);
    }
    else
    {
        winrt::VisualStateManager::GoToState(*this, L"SpinButtonsCollapsed", false);
    }
}

void NumberBox::UpdateSpinButtonEnabled()
{
    const auto value = Value();
    bool isUpButtonEnabled = false;
    bool isDownButtonEnabled = false;

    if (!std::isnan(value))
    {
        if (IsWrapEnabled() || ValidationMode() != winrt::NumberBoxValidationMode::InvalidInputOverwritten)
        {
            // If wrapping is enabled, or invalid values are allowed, then the buttons should be enabled
            isUpButtonEnabled = true;
            isDownButtonEnabled = true;
        }
        else
        {
            if (value < Maximum())
            {
                isUpButtonEnabled = true;
            }
            if (value > Minimum())
            {
                isDownButtonEnabled = true;
            }
        }
    }

    winrt::VisualStateManager::GoToState(*this, isUpButtonEnabled ? L"UpSpinButtonEnabled" : L"UpSpinButtonDisabled", false);
    winrt::VisualStateManager::GoToState(*this, isDownButtonEnabled ? L"DownSpinButtonEnabled" : L"DownSpinButtonDisabled", false);
}

bool NumberBox::IsInBounds(double value)
{
    return (value >= Minimum() && value <= Maximum());
}


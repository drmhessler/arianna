// SPDX-FileCopyrightText: Šimon Rataj <ratajs@ratajs.cz>
// SPDX-License-Identifier: MIT

import QtQuick
import QtQuick.Window
import QtQuick.Controls as QQC2
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.arianna

FormCard.FormCardPage {
    id: root

    title: i18n("Settings")

    FormCard.FormHeader {
        title: i18n("Appearance")
    }

    FormCard.FormCard {
        FormCard.FormSpinBoxDelegate {
            label: i18n("Maximum width:")

            from: Kirigami.Units.smallSpacing
            to: Screen.desktopAvailableWidth
            value: Config.maxWidth
            onValueChanged: {
                Config.maxWidth = value;
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormSpinBoxDelegate {
            label: i18n("Margin:")

            from: 0
            to: Screen.desktopAvailableWidth
            value: Config.margin
            onValueChanged: {
                Config.margin = value;
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormCheckDelegate {
            id: showProgress
            text: i18n("Show progress")

            checked: Config.showProgress
            onCheckedChanged: {
                Config.showProgress = checked;
                Config.save();
            }
        }
    }

    FormCard.FormHeader {
        title: i18n("Font")
    }

    FormCard.FormCard {
        Layout.topMargin: Kirigami.Units.largeSpacing
        Layout.fillWidth: true

        FormCard.FormButtonDelegate {
            text: i18n("Change default font")
            onClicked: fontDialog.open()
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormSpinBoxDelegate {
            label: i18n("Font size:")

            from: 8
            to: 72
            value: Config.fontSize
            onValueChanged: {
                Config.fontSize = value;
                Config.save();
            }
        }

        FormCard.FormSwitchDelegate {
            text: i18n("Use publisher font")

            checked: Config.usePublisherFont
            onCheckedChanged: {
                Config.usePublisherFont = checked;
                Config.save();
            }
        }
    }

    FormCard.FormHeader {
        title: i18n("Text flow")
    }

    FormCard.FormCard {
        FormCard.FormCheckDelegate {
            id: justifyText
            text: i18n("Justify text")

            checked: Config.justify
            onCheckedChanged: {
                Config.justify = checked;
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator { above: justifyText; below: hyphenateText }

        FormCard.FormCheckDelegate {
            id: hyphenateText
            text: i18n("Hyphenate text")

            checked: Config.hyphenate
            onCheckedChanged: {
                Config.hyphenate = checked;
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator { above: hyphenateText }

        FormCard.FormSpinBoxDelegate {
            label: i18n("Line height:")

            from: 10
            to: 50
            textFromValue: (value, locale) => Number(value / 10).toLocaleString(locale, 'f', 1)
            valueFromText: (text, locale) => Number.fromLocaleString(locale, text) * 10

            value: Config.spacing * 10
            onValueChanged: {
                Config.spacing = value / 10
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator { above: hyphenateText }

        FormCard.FormSpinBoxDelegate {
            label: i18n("Brightness:")

            from: 0
            to: 100
            stepSize: 5
            textFromValue: (value, locale) => Number(value / 100).toLocaleString(locale, 'f', 2)
            valueFromText: (text, locale) => Number.fromLocaleString(locale, text) * 100


            value: Config.brightness * 100
            onValueChanged: {
                Config.brightness = value / 100
                Config.save();
            }
        }
    }

    FormCard.FormHeader {
        title: i18n("Colors")
    }

    FormCard.FormCard {
        FormCard.FormSwitchDelegate {
            id: invertColors
            text: i18n("Invert colors")

            checked: Config.invert
            onCheckedChanged: {
                Config.invert = checked
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormSwitchDelegate {
            id: kdeTheming
            text: i18n("KDE Theming")

            checked: Config.kdeTheming
            onCheckedChanged: {
                Config.kdeTheming = checked
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormTextFieldDelegate {
            label: i18n("Reader background image")
            text: Config.readerBackgroundPath
            placeholderText: i18n("No background image")
            onEditingFinished: {
                Config.readerBackgroundPath = text.trim();
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormButtonDelegate {
            text: i18n("Choose background image…")
            icon.name: "document-open"
            onClicked: backgroundImageDialog.open()
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormButtonDelegate {
            text: i18n("Clear background image")
            icon.name: "edit-clear"
            enabled: Config.readerBackgroundPath.length > 0
            onClicked: {
                Config.readerBackgroundPath = "";
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        ColorSchemeDelegate {}
    }

    Dialogs.FileDialog {
        id: backgroundImageDialog

        title: i18n("Choose background image")
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: [i18n("Image files (*.png *.jpg *.jpeg *.webp *.gif *.svg)")]
        onAccepted: {
            Config.readerBackgroundPath = selectedFile.toString();
            Config.save();
        }
    }

    FormCard.FormHeader {
        title: i18n("Translation")
    }

    FormCard.FormCard {
        FormCard.FormComboBoxDelegate {
            text: i18n("Translator")
            textRole: "display"
            valueRole: "value"
            model: [
                { display: i18n("Google Translate"), value: 0 },
                { display: i18n("DeepL"), value: 1 }
            ]
            currentIndex: Config.translatorEngine
            onActivated: index => {
                Config.translatorEngine = model[index].value;
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormTextFieldDelegate {
            label: i18n("Target language")
            text: Config.targetLanguage
            placeholderText: i18n("DE")
            inputMethodHints: Qt.ImhUppercaseOnly | Qt.ImhNoPredictiveText
            onEditingFinished: {
                Config.targetLanguage = text.trim() || "DE";
                text = Config.targetLanguage;
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormPasswordFieldDelegate {
            label: i18n("Google API key")
            text: Config.googleApiKey
            placeholderText: i18n("Optional")
            onEditingFinished: {
                Config.googleApiKey = text.trim();
                Config.save();
            }
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormPasswordFieldDelegate {
            label: i18n("DeepL API key")
            text: Config.deeplApiKey
            enabled: Config.translatorEngine === 1
            placeholderText: i18n("Required for DeepL")
            onEditingFinished: {
                Config.deeplApiKey = text.trim();
                Config.save();
            }
        }
    }

    FormCard.FormCard {
        Layout.topMargin: Kirigami.Units.largeSpacing

        FormCard.FormButtonDelegate {
            text: i18n("About Arianna")
            onClicked: applicationWindow().pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutPage"))
            icon.name: "org.kde.arianna"
        }

        FormCard.FormDelegateSeparator {}

        FormCard.FormButtonDelegate {
            text: i18n("About KDE")
            onClicked: applicationWindow().pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutKDEPage"))
            icon.name: "kde"
        }
    }

    data: Dialogs.FontDialog {
        id: fontDialog

        title: i18n("Change default font")

        currentFont: Config.defaultFont

        onAccepted: {
            Config.defaultFont = fontDialog.selectedFont;
            Config.save();
        }
        onRejected: fontDialog.currentFont = Config.defaultFont;
    }
}

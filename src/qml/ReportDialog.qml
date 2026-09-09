// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

QQC2.Dialog {
    id: root

    property var report: ({})
    property string rawXml: ""
    readonly property bool reportValid: report && report.valid === true
    readonly property var rules: reportValid && report.rules ? report.rules : []

    modal: true
    focus: true
    padding: 0
    title: i18nc("@title:window", "PDF Validation")
    standardButtons: QQC2.Dialog.NoButton
    implicitWidth: Kirigami.Units.gridUnit * 52
    implicitHeight: Kirigami.Units.gridUnit * 48
    width: parent ? Math.min(implicitWidth, Math.max(Kirigami.Units.gridUnit * 16, parent.width - Kirigami.Units.gridUnit * 2)) : implicitWidth
    height: parent ? Math.min(implicitHeight, Math.max(Kirigami.Units.gridUnit * 16, parent.height - Kirigami.Units.gridUnit * 2)) : implicitHeight

    function deviationsText(count) {
        return i18np("%1 deviation", "%1 deviations", count);
    }

    function failedRulesText(count) {
        return i18np("%1 failed rule", "%1 failed rules", count);
    }

    function errorsText(count) {
        return i18np("%1 error", "%1 errors", count);
    }

    function ruleTitle(rule) {
        return rule.clause || rule.object || i18n("Unnamed rule");
    }

    contentItem: QQC2.ScrollView {
        id: reportScrollView

        clip: true
        QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
        QQC2.ScrollBar.vertical.policy: QQC2.ScrollBar.AsNeeded

        ColumnLayout {
            width: reportScrollView.availableWidth
            spacing: Kirigami.Units.largeSpacing

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.smallSpacing
            }

            ColumnLayout {
                visible: root.reportValid
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    text: i18n("PDF Validation")
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.report.compliant ? i18n("✓ Compliant") : i18n("✗ Not compliant")
                    color: root.report.compliant ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.negativeTextColor
                    font.bold: true
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.25
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.report.profileName || i18n("Arlington profile")
                    font.bold: true
                    wrapMode: Text.Wrap
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.report.displayFileName || root.report.fileName
                    elide: Text.ElideMiddle
                    visible: text.length > 0
                    QQC2.ToolTip.visible: hovered && root.report.fileName.length > 0
                    QQC2.ToolTip.text: root.report.fileName
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Label {
                        text: root.deviationsText(root.report.deviations || 0)
                    }

                    QQC2.Label {
                        text: "·"
                    }

                    QQC2.Label {
                        text: root.failedRulesText(root.report.failedRules || 0)
                    }
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18n("Duration: %1 s", Number(root.report.durationSeconds).toFixed(3))
                    visible: Number(root.report.durationSeconds) >= 0
                }
            }

            Kirigami.Separator {
                visible: root.reportValid
                Layout.fillWidth: true
            }

            QQC2.Label {
                visible: root.reportValid && root.rules.length === 0
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                text: i18n("No failed rules were reported.")
                wrapMode: Text.Wrap
            }

            Repeater {
                model: root.rules

                delegate: QQC2.Frame {
                    id: ruleCard

                    required property var modelData
                    property var rule: modelData
                    property bool expanded: false

                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    padding: Kirigami.Units.largeSpacing

                    ColumnLayout {
                        width: parent.width
                        spacing: Kirigami.Units.smallSpacing

                        QQC2.Button {
                            Layout.fillWidth: true
                            flat: true
                            icon.name: ruleCard.expanded ? "arrow-down" : "arrow-right"
                            text: root.ruleTitle(ruleCard.rule) + " · " + root.errorsText((ruleCard.rule.checks || []).length)
                            font.bold: true
                            onClicked: ruleCard.expanded = !ruleCard.expanded
                        }

                        QQC2.Label {
                            visible: ruleCard.expanded && ruleCard.rule.description.length > 0
                            Layout.fillWidth: true
                            text: ruleCard.rule.description
                            wrapMode: Text.Wrap
                        }

                        GridLayout {
                            visible: ruleCard.expanded
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: Kirigami.Units.largeSpacing
                            rowSpacing: Kirigami.Units.smallSpacing

                            QQC2.Label {
                                text: i18n("Specification:")
                                font.bold: true
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: ruleCard.rule.specification
                                wrapMode: Text.Wrap
                            }
                            QQC2.Label {
                                text: i18n("Clause:")
                                font.bold: true
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: ruleCard.rule.clause
                                wrapMode: Text.Wrap
                            }
                            QQC2.Label {
                                text: i18n("Test number:")
                                font.bold: true
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: ruleCard.rule.testNumber
                            }
                            QQC2.Label {
                                text: i18n("Object type:")
                                font.bold: true
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: ruleCard.rule.object
                                wrapMode: Text.Wrap
                            }
                            QQC2.Label {
                                text: i18n("Deviations:")
                                font.bold: true
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: ruleCard.rule.deviations
                            }
                        }

                        Repeater {
                            visible: ruleCard.expanded
                            model: ruleCard.rule.checks || []

                            delegate: QQC2.Frame {
                                id: checkCard

                                required property var modelData
                                property var check: modelData
                                property bool technicalDetailsVisible: false

                                Layout.fillWidth: true
                                padding: Kirigami.Units.largeSpacing

                                ColumnLayout {
                                    width: parent.width
                                    spacing: Kirigami.Units.smallSpacing

                                    QQC2.Label {
                                        Layout.fillWidth: true
                                        text: checkCard.check.pdfObjectNumber.length > 0 ? i18n("✗ Object %1", checkCard.check.pdfObjectNumber) : i18n("✗ Failed check")
                                        color: Kirigami.Theme.negativeTextColor
                                        font.bold: true
                                    }

                                    TextEdit {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: contentHeight
                                        text: checkCard.check.errorMessage
                                        color: Kirigami.Theme.textColor
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: TextEdit.Wrap
                                    }

                                    QQC2.Button {
                                        visible: checkCard.check.context.length > 0
                                        flat: true
                                        icon.name: checkCard.technicalDetailsVisible ? "arrow-down" : "arrow-right"
                                        text: i18n("Technical Details")
                                        onClicked: checkCard.technicalDetailsVisible = !checkCard.technicalDetailsVisible
                                    }

                                    QQC2.Label {
                                        visible: checkCard.technicalDetailsVisible && checkCard.check.context.length > 0
                                        Layout.fillWidth: true
                                        text: i18n("Context:")
                                        font.bold: true
                                    }

                                    QQC2.ScrollView {
                                        id: contextScrollView

                                        visible: checkCard.technicalDetailsVisible && checkCard.check.context.length > 0
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 10,
                                            Math.max(Kirigami.Units.gridUnit * 4, contextArea.contentHeight + contextArea.topPadding + contextArea.bottomPadding))
                                        clip: true
                                        QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
                                        QQC2.ScrollBar.vertical.policy: QQC2.ScrollBar.AsNeeded

                                        QQC2.TextArea {
                                            id: contextArea

                                            width: contextScrollView.availableWidth
                                            text: checkCard.check.context
                                            textFormat: TextEdit.PlainText
                                            font.family: "monospace"
                                            readOnly: true
                                            selectByMouse: true
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            QQC2.Frame {
                id: invalidReportCard

                visible: !root.reportValid
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                padding: Kirigami.Units.largeSpacing

                QQC2.Label {
                    width: parent.width
                    text: i18n("The PDF check output is not a valid Arlington XML report.")
                    wrapMode: Text.Wrap
                }
            }

            QQC2.Frame {
                id: rawXmlCard

                visible: root.rawXml.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                property bool expanded: false
                padding: Kirigami.Units.largeSpacing

                ColumnLayout {
                    width: parent.width
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Button {
                        Layout.fillWidth: true
                        flat: true
                        icon.name: rawXmlCard.expanded ? "arrow-down" : "arrow-right"
                        text: i18n("Raw XML (Debug)")
                        onClicked: rawXmlCard.expanded = !rawXmlCard.expanded
                    }

                    QQC2.ScrollView {
                        id: rawXmlScrollView

                        visible: rawXmlCard.expanded
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 16,
                            Math.max(Kirigami.Units.gridUnit * 6, rawXmlArea.contentHeight + rawXmlArea.topPadding + rawXmlArea.bottomPadding))
                        clip: true
                        QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AsNeeded
                        QQC2.ScrollBar.vertical.policy: QQC2.ScrollBar.AsNeeded

                        QQC2.TextArea {
                            id: rawXmlArea

                            width: rawXmlScrollView.availableWidth
                            text: root.rawXml
                            textFormat: TextEdit.PlainText
                            font.family: "monospace"
                            readOnly: true
                            selectByMouse: true
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.smallSpacing
            }
        }
    }
}

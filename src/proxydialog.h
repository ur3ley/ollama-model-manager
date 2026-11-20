#ifndef PROXYDIALOG_H
#define PROXYDIALOG_H

#include "proxysettings.h"

#include <QDialog>

class ProxyService;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QWidget;

/**
 * Настройки встроенного прокси: режим, адреса, порт, апстрим, TLS.
 *
 * «Применить» проверяет bind (после остановки текущего экземпляра — иначе
 * собственный сокет выглядел бы как «порт занят»), сохраняет файл и
 * перезапускает ProxyService. При ошибке диалог не закрывается.
 */
class ProxyDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProxyDialog(ProxyService *service, QWidget *parent = nullptr);

private slots:
    void onApply();
    void updateHint();
    void updateStatus();
    void updateTlsUi();
    void onExportCa();
    void onRenewCertificate();

private:
    ProxySettings settingsFromForm() const;
    void populateAddresses(const ProxySettings &settings);
    void setResult(const QString &text, bool error = false);

    ProxyService *m_service;
    ProxySettings m_settings;

    QCheckBox *m_enableCheck;
    QRadioButton *m_interfacesRadio;
    QRadioButton *m_allRadio;
    QListWidget *m_addressList;
    QSpinBox *m_portSpin;
    QLineEdit *m_upstreamHost;
    QSpinBox *m_upstreamPortSpin;
    QLabel *m_hintLabel;
    QLabel *m_statusLabel;
    QLabel *m_resultLabel;
    QPushButton *m_applyButton;

    QComboBox *m_tlsCombo;
    QWidget *m_tlsFilesRow;      // сертификат + ключ (только для Files)
    QLineEdit *m_tlsCertEdit;
    QLineEdit *m_tlsKeyEdit;
    QWidget *m_tlsCaRow;         // путь к CA + экспорт (только для Internal)
    QLabel *m_tlsCaLabel;
    QLabel *m_tlsExpiryLabel;
    QLabel *m_tlsNoteLabel;
    QPushButton *m_exportCaButton;
    QPushButton *m_renewButton;
};

#endif // PROXYDIALOG_H

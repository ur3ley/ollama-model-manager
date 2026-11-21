#include "proxydialog.h"

#include "designtokens.h"
#include "i18n.h"
#include "proxyservice.h"
#include "tlsmanager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

quint16 suggestedPort(quint16 port)
{
    return port >= 65535 ? quint16(65534) : quint16(port + 1);
}

ProxySettings::TlsMode tlsModeAt(int index)
{
    switch (index) {
    case 0:
        return ProxySettings::TlsMode::Off;
    case 2:
        return ProxySettings::TlsMode::Files;
    case 3:
        return ProxySettings::TlsMode::LetsEncrypt;
    case 1:
    default:
        return ProxySettings::TlsMode::Internal;
    }
}

int tlsModeIndex(ProxySettings::TlsMode mode)
{
    switch (mode) {
    case ProxySettings::TlsMode::Off:
        return 0;
    case ProxySettings::TlsMode::Files:
        return 2;
    case ProxySettings::TlsMode::LetsEncrypt:
        return 3;
    case ProxySettings::TlsMode::Internal:
        break;
    }
    return 1;
}

} // namespace

ProxyDialog::ProxyDialog(ProxyService *service, QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_settings(service ? service->settings() : ProxySettings::load())
{
    setWindowTitle(T("proxy.title"));
    resize(560, 700);

    m_enableCheck = new QCheckBox(T("proxy.enable"), this);
    m_enableCheck->setChecked(m_settings.enabled);

    auto *modeGroup = new QGroupBox(T("proxy.mode"), this);
    m_interfacesRadio = new QRadioButton(T("proxy.modeInterfaces"), modeGroup);
    m_allRadio = new QRadioButton(T("proxy.modeAll"), modeGroup);
    auto *modeLayout = new QVBoxLayout(modeGroup);
    modeLayout->addWidget(m_interfacesRadio);
    modeLayout->addWidget(m_allRadio);

    m_addressList = new QListWidget(this);
    m_addressList->setSelectionMode(QAbstractItemView::NoSelection);
    populateAddresses(m_settings);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(m_settings.port);

    m_upstreamHost = new QLineEdit(m_settings.upstreamHost, this);
    m_upstreamHost->setPlaceholderText(QStringLiteral("127.0.0.1"));

    m_upstreamPortSpin = new QSpinBox(this);
    m_upstreamPortSpin->setRange(1, 65535);
    m_upstreamPortSpin->setValue(m_settings.upstreamPort);

    auto *upstreamGroup = new QGroupBox(T("proxy.upstream"), this);
    auto *upstreamForm = new QFormLayout(upstreamGroup);
    upstreamForm->addRow(T("proxy.host"), m_upstreamHost);
    upstreamForm->addRow(T("proxy.upstreamPort"), m_upstreamPortSpin);
    auto *portForm = new QFormLayout;
    portForm->addRow(T("proxy.port"), m_portSpin);

    // --- TLS ---------------------------------------------------------------
    auto *tlsGroup = new QGroupBox(T("proxy.tls"), this);
    auto *tlsForm = new QFormLayout(tlsGroup);

    m_tlsCombo = new QComboBox(tlsGroup);
    m_tlsCombo->addItem(T("proxy.tlsOff"));
    m_tlsCombo->addItem(T("proxy.tlsInternal"));
    m_tlsCombo->addItem(T("proxy.tlsFiles"));
    m_tlsCombo->addItem(T("proxy.tlsLetsEncrypt"));
    m_tlsCombo->setCurrentIndex(tlsModeIndex(m_settings.tls));
    tlsForm->addRow(T("proxy.tlsMode"), m_tlsCombo);

    m_tlsFilesRow = new QWidget(tlsGroup);
    auto *filesForm = new QFormLayout(m_tlsFilesRow);
    filesForm->setContentsMargins(0, 0, 0, 0);
    m_tlsCertEdit = new QLineEdit(m_settings.tlsCert, m_tlsFilesRow);
    m_tlsKeyEdit = new QLineEdit(m_settings.tlsKey, m_tlsFilesRow);
    auto *certRow = new QHBoxLayout;
    certRow->addWidget(m_tlsCertEdit, 1);
    auto *certButton = new QPushButton(QStringLiteral("…"), m_tlsFilesRow);
    certButton->setFixedWidth(32);
    certRow->addWidget(certButton);
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_tlsKeyEdit, 1);
    auto *keyButton = new QPushButton(QStringLiteral("…"), m_tlsFilesRow);
    keyButton->setFixedWidth(32);
    keyRow->addWidget(keyButton);
    filesForm->addRow(T("proxy.tlsCert"), certRow);
    filesForm->addRow(T("proxy.tlsKey"), keyRow);
    tlsForm->addRow(m_tlsFilesRow);

    m_tlsCaRow = new QWidget(tlsGroup);
    auto *caRow = new QHBoxLayout(m_tlsCaRow);
    caRow->setContentsMargins(0, 0, 0, 0);
    m_tlsCaLabel = new QLabel(TlsManager::caCertPath(), m_tlsCaRow);
    m_tlsCaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_tlsCaLabel->setWordWrap(true);
    m_exportCaButton = new QPushButton(T("proxy.tlsExportCa"), m_tlsCaRow);
    caRow->addWidget(m_tlsCaLabel, 1);
    caRow->addWidget(m_exportCaButton);
    tlsForm->addRow(m_tlsCaRow);

    m_tlsExpiryLabel = new QLabel(this);
    tlsForm->addRow(T("proxy.tlsExpiry"), m_tlsExpiryLabel);

    m_renewButton = new QPushButton(T("proxy.tlsRenew"), this);
    tlsForm->addRow(QString(), m_renewButton);

    m_tlsNoteLabel = new QLabel(this);
    m_tlsNoteLabel->setWordWrap(true);
    tokens::setMuted(m_tlsNoteLabel);
    tlsForm->addRow(m_tlsNoteLabel);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setWordWrap(true);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);

    m_resultLabel = new QLabel(this);
    m_resultLabel->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(this);
    m_applyButton = buttons->addButton(T("proxy.apply"), QDialogButtonBox::AcceptRole);
    QPushButton *cancelButton = buttons->addButton(T("common.cancel"), QDialogButtonBox::RejectRole);
    m_applyButton->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_enableCheck);
    layout->addWidget(modeGroup);
    layout->addWidget(new QLabel(T("proxy.addresses"), this));
    layout->addWidget(m_addressList, 1);
    layout->addLayout(portForm);
    layout->addWidget(upstreamGroup);
    layout->addWidget(tlsGroup);
    layout->addWidget(m_hintLabel);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_resultLabel);
    layout->addWidget(buttons);

    m_interfacesRadio->setChecked(m_settings.mode == ProxySettings::Interfaces);
    m_allRadio->setChecked(m_settings.mode == ProxySettings::AllInterfaces);

    connect(m_applyButton, &QPushButton::clicked, this, &ProxyDialog::onApply);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_enableCheck, &QCheckBox::toggled, this, &ProxyDialog::updateHint);
    connect(m_interfacesRadio, &QRadioButton::toggled, this, &ProxyDialog::updateHint);
    connect(m_allRadio, &QRadioButton::toggled, this, &ProxyDialog::updateHint);
    connect(m_addressList, &QListWidget::itemChanged, this, &ProxyDialog::updateHint);
    connect(m_portSpin, &QSpinBox::valueChanged, this, &ProxyDialog::updateHint);
    connect(m_upstreamHost, &QLineEdit::textChanged, this, &ProxyDialog::updateHint);
    connect(m_upstreamPortSpin, &QSpinBox::valueChanged, this, &ProxyDialog::updateHint);
    connect(m_tlsCombo, &QComboBox::currentIndexChanged, this, &ProxyDialog::updateTlsUi);
    connect(m_tlsCertEdit, &QLineEdit::textChanged, this, &ProxyDialog::updateHint);
    connect(m_tlsKeyEdit, &QLineEdit::textChanged, this, &ProxyDialog::updateHint);
    connect(m_exportCaButton, &QPushButton::clicked, this, &ProxyDialog::onExportCa);
    connect(m_renewButton, &QPushButton::clicked, this, &ProxyDialog::onRenewCertificate);
    connect(certButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, T("proxy.tlsChooseCert"), QFileInfo(m_tlsCertEdit->text()).absolutePath(),
            QStringLiteral("*.crt *.pem *.cer"));
        if (!path.isEmpty())
            m_tlsCertEdit->setText(path);
    });
    connect(keyButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, T("proxy.tlsChooseKey"), QFileInfo(m_tlsKeyEdit->text()).absolutePath(),
            QStringLiteral("*.key *.pem"));
        if (!path.isEmpty())
            m_tlsKeyEdit->setText(path);
    });
    if (m_service) {
        connect(m_service, &ProxyService::statusChanged, this, &ProxyDialog::updateStatus);
        connect(m_service, &ProxyService::tlsChanged, this, [this](const QDateTime &) {
            updateTlsUi();
        });
    }

    updateTlsUi();
    updateHint();
    updateStatus();
}

void ProxyDialog::populateAddresses(const ProxySettings &settings)
{
    QStringList seen;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QString name = iface.humanReadableName().isEmpty() ? iface.name()
                                                                 : iface.humanReadableName();
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.isNull() || address.isLoopback() || ProxySettings::isLinkLocal(address))
                continue;
            const QString text = address.toString();
            if (seen.contains(text))
                continue;
            seen.append(text);

            auto *item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(name, text),
                                             m_addressList);
            item->setData(Qt::UserRole, text);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(settings.addresses.contains(text) ? Qt::Checked : Qt::Unchecked);
        }
    }

    // Сохранённый адрес мог исчезнуть (интерфейс выключен) — не теряем его.
    for (const QString &address : settings.addresses) {
        if (seen.contains(address))
            continue;
        auto *item = new QListWidgetItem(address, m_addressList);
        item->setData(Qt::UserRole, address);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
}

ProxySettings ProxyDialog::settingsFromForm() const
{
    ProxySettings settings = m_settings;
    settings.enabled = m_enableCheck->isChecked();
    settings.mode = m_allRadio->isChecked() ? ProxySettings::AllInterfaces
                                            : ProxySettings::Interfaces;

    settings.addresses.clear();
    for (int i = 0; i < m_addressList->count(); ++i) {
        const QListWidgetItem *item = m_addressList->item(i);
        if (item->checkState() == Qt::Checked)
            settings.addresses.append(item->data(Qt::UserRole).toString());
    }

    settings.port = quint16(m_portSpin->value());
    settings.upstreamHost = m_upstreamHost->text().trimmed();
    if (settings.upstreamHost.isEmpty())
        settings.upstreamHost = QStringLiteral("127.0.0.1");
    settings.upstreamPort = quint16(m_upstreamPortSpin->value());

    settings.tls = tlsModeAt(m_tlsCombo->currentIndex());
    settings.tlsCert = m_tlsCertEdit->text().trimmed();
    settings.tlsKey = m_tlsKeyEdit->text().trimmed();
    return settings;
}

void ProxyDialog::updateTlsUi()
{
    const ProxySettings::TlsMode mode = tlsModeAt(m_tlsCombo->currentIndex());
    m_tlsFilesRow->setVisible(mode == ProxySettings::TlsMode::Files);
    m_tlsCaRow->setVisible(mode == ProxySettings::TlsMode::Internal);
    m_exportCaButton->setVisible(mode == ProxySettings::TlsMode::Internal);
    m_renewButton->setVisible(mode == ProxySettings::TlsMode::Internal);

    const QDateTime expiry = m_service ? m_service->certificateExpiry() : QDateTime();
    const bool valid = expiry.isValid();
    const bool soon = valid
                      && expiry.toUTC() <= QDateTime::currentDateTimeUtc().addDays(14);
    if (valid) {
        m_tlsExpiryLabel->setText(expiry.toLocalTime().toString(QStringLiteral("yyyy-MM-dd")));
        tokens::setRole(m_tlsExpiryLabel, soon ? "danger" : "");
    } else {
        m_tlsExpiryLabel->setText(T("proxy.tlsExpiryUnknown"));
        tokens::setRole(m_tlsExpiryLabel, "");
    }

    if (mode == ProxySettings::TlsMode::LetsEncrypt)
        m_tlsNoteLabel->setText(T("proxy.tlsLetsEncryptHint"));
    else if (mode == ProxySettings::TlsMode::Internal)
        m_tlsNoteLabel->setText(T("proxy.tlsInternalHint"));
    else
        m_tlsNoteLabel->clear();

    updateHint();
}

void ProxyDialog::updateHint()
{
    const ProxySettings settings = settingsFromForm();
    m_addressList->setEnabled(m_interfacesRadio->isChecked());

    QString hint;
    bool error = false;
    if (settings.enabled) {
        if (settings.mode == ProxySettings::Interfaces && settings.addresses.isEmpty()) {
            hint = T("proxy.errNoAddress");
            error = true;
        } else if (settings.conflictsWithLocalUpstream()) {
            hint = T("proxy.errPortOllama")
                       .arg(settings.port)
                       .arg(suggestedPort(settings.port));
            error = true;
        } else if (settings.tls == ProxySettings::TlsMode::Files
                   && (settings.tlsCert.isEmpty() || settings.tlsKey.isEmpty())) {
            hint = T("proxy.tlsErrNoCert");
            error = true;
        } else if (settings.tls == ProxySettings::TlsMode::LetsEncrypt) {
            // Честно: режим сохранён в настройках, но пока не реализован.
            hint = T("proxy.tlsLetsEncryptHint");
            error = true;
        }
    }

    tokens::setRole(m_hintLabel, error ? "danger" : "");
    tokens::setMuted(m_hintLabel, !error);
    m_hintLabel->setText(hint.isEmpty() ? T("proxy.describe").arg(settings.describe()) : hint);
    m_applyButton->setEnabled(!error);
}

void ProxyDialog::updateStatus()
{
    m_statusLabel->setText(T("proxy.status").arg(m_service ? m_service->statusText()
                                                           : T("proxy.statusStopped")));
    updateTlsUi();
}

void ProxyDialog::onExportCa()
{
    const QString defaultName =
        QDir(QDir::homePath()).filePath(QStringLiteral("ollama-manager-ca.crt"));
    const QString target = QFileDialog::getSaveFileName(this, T("proxy.tlsExportTitle"),
                                                        defaultName, QStringLiteral("*.crt"));
    if (target.isEmpty())
        return;

    TlsManager tls;
    QString error;
    if (!tls.exportCa(target, &error)) {
        setResult(T("proxy.tlsErrExportCa").arg(error), true);
        return;
    }
    setResult(T("proxy.tlsExported").arg(target));
}

void ProxyDialog::onRenewCertificate()
{
    if (!m_service) {
        setResult(T("proxy.errRestart").arg(QStringLiteral("ProxyService is not available")), true);
        return;
    }
    QString error;
    if (!m_service->renewCertificate(&error)) {
        setResult(T("proxy.tlsErrRenew").arg(error), true);
        return;
    }
    setResult(T("proxy.tlsRenewed"));
    updateTlsUi();
    updateStatus();
}

void ProxyDialog::setResult(const QString &text, bool error)
{
    tokens::setRole(m_resultLabel, error ? "danger" : "success");
    m_resultLabel->setText(text);
}

void ProxyDialog::onApply()
{
    if (!m_service) {
        setResult(T("proxy.errRestart").arg(QStringLiteral("ProxyService is not available")), true);
        return;
    }
    const ProxySettings settings = settingsFromForm();
    QString error;

    // restart() останавливает текущий экземпляр и валидирует bind уже без
    // собственного сокета; иначе «порт занят» срабатывал бы на самого себя.
    if (settings.enabled) {
        if (!m_service->restart(settings, &error)) {
            setResult(T("proxy.errRestart").arg(error), true);
            return;
        }
    } else {
        m_service->stop();
    }

    if (!settings.save(&error)) {
        setResult(T("proxy.errSave").arg(error), true);
        return;
    }

    m_settings = settings;
    setResult(T("proxy.applied"));
    updateHint();
    updateStatus();
}

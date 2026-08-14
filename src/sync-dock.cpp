/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * sync-dock.cpp — the IRL Sync dock.
 *
 * The only C++ in the plugin, and the only part that needs Qt. It reads the
 * registry in sync-group.c and writes the three global settings; it never
 * touches a source's internals, so nothing here can race the media path.
 *
 * obs-frontend-api is resolved at runtime rather than linked. The CI builds
 * libobs alone out of a stripped OBS tree — un-stripping it to get
 * obs-frontend-api would drag the entire Qt frontend into the dependency
 * build — and the two symbols needed here are already in the process by the
 * time any plugin loads. Resolving them dynamically also means a headless OBS
 * simply gets no dock instead of failing to load the module, which is the same
 * bargain the websocket vendor already makes with obs-websocket.
 */

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <obs-module.h>

#include "../include/irl-ntp.h"
#include "../include/irl-sync.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define IRL_SYNC_DOCK_ID "irl-sync-dock"

/* Several missed polls (the client refreshes about once a minute). Past this
 * the offset is old enough that everything derived from it is suspect. */
static constexpr quint64 NTP_STALE_AGE_NS = 300ULL * 1000000000ULL;

/* ── Frontend entry points ────────────────────────────────── */

typedef bool (*add_dock_by_id_fn)(const char *id, const char *title,
				  void *widget);
typedef void (*remove_dock_fn)(const char *id);

static void *resolve_frontend_symbol(const char *name)
{
#ifdef _WIN32
	HMODULE module = GetModuleHandleA("obs-frontend-api.dll");
	if (!module)
		return nullptr;
	return reinterpret_cast<void *>(GetProcAddress(module, name));
#else
	/* RTLD_DEFAULT searches everything already loaded, which is where
	 * obs-frontend-api is: the UI links it before any plugin loads. */
	return dlsym(RTLD_DEFAULT, name);
#endif
}

/* ── Formatting ───────────────────────────────────────────── */

static QString format_ms(int64_t ms)
{
	if (ms > -1000 && ms < 1000)
		return QString("%1 ms").arg(ms);
	return QString("%1 s").arg(static_cast<double>(ms) / 1000.0, 0, 'f', 2);
}

static QString format_timecode(const struct irl_sync_snapshot &snap)
{
	if (!snap.have_timecode)
		return QStringLiteral("—");

	return QString("%1:%2:%3:%4")
		.arg(snap.tc.hours, 2, 10, QChar('0'))
		.arg(snap.tc.minutes, 2, 10, QChar('0'))
		.arg(snap.tc.seconds, 2, 10, QChar('0'))
		.arg(snap.tc.n_frames, 2, 10, QChar('0'));
}

/* Colour and wording per state. Four states rather than red/green, because
 * they call for different actions: "No timecode" is a sender configuration
 * problem the offset cannot fix, "Acquiring" is the normal path to Locked and
 * must not read as a fault, and only "Too slow" is something the operator can
 * act on here — so that is the one that says what to do. */
static QColor status_colour(enum irl_sync_status status)
{
	switch (status) {
	case IRL_SYNC_LOCKED:
		return QColor(39, 174, 96);
	case IRL_SYNC_ACQUIRING:
		return QColor(214, 137, 16);
	case IRL_SYNC_TOO_SLOW:
		return QColor(192, 57, 43);
	case IRL_SYNC_STALE:
		return QColor(146, 43, 33);
	case IRL_SYNC_NO_TIMECODE:
	case IRL_SYNC_OFF:
	default:
		return QColor(127, 140, 141);
	}
}

static QString status_text(const struct irl_sync_snapshot &snap)
{
	switch (snap.status) {
	case IRL_SYNC_LOCKED:
		return QStringLiteral("● Locked");
	case IRL_SYNC_ACQUIRING:
		return QStringLiteral("● Acquiring…");
	case IRL_SYNC_TOO_SLOW:
		/* The number is the point: it tells the operator whether to
		 * raise the offset themselves or call the person in the field. */
		return QString("▲ Too slow — needs ≥ %1")
			.arg(format_ms(snap.required_offset_ms));
	case IRL_SYNC_STALE:
		return QStringLiteral("▲ No data");
	case IRL_SYNC_NO_TIMECODE:
		return QStringLiteral("○ No timecode");
	case IRL_SYNC_OFF:
	default:
		return QStringLiteral("○ Off");
	}
}

/* ── Dock widget ──────────────────────────────────────────── */

namespace {

enum column {
	COL_SOURCE = 0,
	COL_SYNC,
	COL_TIMECODE,
	COL_LATENCY,
	COL_PEAK,
	COL_ADDED,
	COL_ERROR,
	COL_STATUS,
	COL_COUNT,
};

class SyncDock : public QWidget {
public:
	explicit SyncDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		build();

		/* 5Hz. Fast enough that the clock's milliseconds read as
		 * moving, slow enough that the numbers stay legible and the
		 * dock costs nothing. */
		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this,
				 [this]() { refresh(); });
		timer->start(200);
	}

private:
	QLabel *clockLabel = nullptr;
	QLabel *ntpLabel = nullptr;
	QLabel *showingLabel = nullptr;
	QLabel *recommendedLabel = nullptr;
	QLabel *summaryLabel = nullptr;
	QSpinBox *offsetSpin = nullptr;
	QLineEdit *ntpEdit = nullptr;
	QPushButton *masterButton = nullptr;
	QPushButton *autoButton = nullptr;
	QTableWidget *table = nullptr;

	int blinkTicks = 0;
	bool blinkOn = false;
	int64_t recommendedMs = 0;

	void build()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(10, 10, 10, 10);
		root->setSpacing(6);

		clockLabel = new QLabel(QStringLiteral("--:--:--.---"), this);
		QFont clockFont = clockLabel->font();
		clockFont.setPointSize(clockFont.pointSize() + 14);
		clockFont.setBold(true);
		/* The clock and every latency figure are numbers that change
		 * in place; a proportional font makes them jitter sideways. */
		clockFont.setStyleHint(QFont::Monospace);
		clockLabel->setFont(clockFont);
		clockLabel->setAlignment(Qt::AlignCenter);
		root->addWidget(clockLabel);

		ntpLabel = new QLabel(this);
		ntpLabel->setAlignment(Qt::AlignCenter);
		root->addWidget(ntpLabel);

		/* Without this line the big clock and the source timecodes
		 * differ by exactly the offset and look like a bug. */
		showingLabel = new QLabel(this);
		showingLabel->setAlignment(Qt::AlignCenter);
		root->addWidget(showingLabel);

		root->addSpacing(4);
		root->addLayout(buildControls());

		summaryLabel = new QLabel(this);
		summaryLabel->setAlignment(Qt::AlignCenter);
		summaryLabel->hide();
		root->addWidget(summaryLabel);

		buildTable();
		root->addWidget(table, 1);
	}

	QHBoxLayout *buildControls()
	{
		auto *row = new QHBoxLayout();
		row->setSpacing(6);

		row->addWidget(new QLabel(QStringLiteral("Offset"), this));

		offsetSpin = new QSpinBox(this);
		offsetSpin->setRange(IRL_SYNC_MIN_OFFSET_MS,
				     IRL_SYNC_MAX_OFFSET_MS);
		offsetSpin->setSingleStep(250);
		offsetSpin->setSuffix(QStringLiteral(" ms"));
		offsetSpin->setValue(irl_sync_offset_ms());
		/* Named on the declaring class: editingFinished and clicked
		 * are inherited, and spelling the base out keeps the
		 * pointer-to-member unambiguous. */
		QObject::connect(offsetSpin,
				 &QAbstractSpinBox::editingFinished, this,
				 [this]() {
					 irl_sync_set_offset_ms(
						 offsetSpin->value());
				 });
		row->addWidget(offsetSpin);

		autoButton = new QPushButton(QStringLiteral("Auto"), this);
		autoButton->setToolTip(QStringLiteral(
			"Set the offset from the worst synced feed's peak "
			"arrival latency, plus margin."));
		QObject::connect(autoButton, &QAbstractButton::clicked, this,
				 [this]() {
					 if (recommendedMs > 0)
						 irl_sync_set_offset_ms(
							 static_cast<int>(
								 recommendedMs));
				 });
		row->addWidget(autoButton);

		recommendedLabel = new QLabel(this);
		row->addWidget(recommendedLabel);

		row->addSpacing(12);
		row->addWidget(new QLabel(QStringLiteral("NTP"), this));

		ntpEdit = new QLineEdit(this);
		ntpEdit->setPlaceholderText(
			QStringLiteral(IRL_SYNC_DEFAULT_NTP_SERVER));
		/* Both ends have to be on a comparable reference, so this is
		 * the one setting co-streamers have to read to each other. */
		ntpEdit->setToolTip(QStringLiteral(
			"Must be a reference the senders also use. Set the "
			"same pool here and in each Moblin device."));
		QObject::connect(ntpEdit, &QLineEdit::editingFinished, this,
				 [this]() {
					 irl_sync_set_ntp_server(
						 ntpEdit->text()
							 .trimmed()
							 .toUtf8()
							 .constData());
				 });
		row->addWidget(ntpEdit, 1);

		masterButton = new QPushButton(this);
		masterButton->setCheckable(true);
		QObject::connect(masterButton, &QAbstractButton::clicked, this,
				 [this](bool checked) {
					 irl_sync_set_enabled(checked);
					 updateMasterButton(checked);
				 });
		row->addWidget(masterButton);

		return row;
	}

	void buildTable()
	{
		table = new QTableWidget(0, COL_COUNT, this);
		table->setHorizontalHeaderLabels(
			{QStringLiteral("Source"), QStringLiteral("Sync"),
			 QStringLiteral("Timecode"), QStringLiteral("Latency"),
			 QStringLiteral("Peak 60s"), QStringLiteral("Added"),
			 QStringLiteral("Error"), QStringLiteral("Status")});
		table->verticalHeader()->setVisible(false);
		table->setSelectionMode(QAbstractItemView::NoSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->setAlternatingRowColors(true);
		table->horizontalHeader()->setSectionResizeMode(
			COL_SOURCE, QHeaderView::Stretch);
		for (int i = COL_SYNC; i < COL_COUNT; i++) {
			table->horizontalHeader()->setSectionResizeMode(
				i, QHeaderView::ResizeToContents);
		}
	}

	void updateMasterButton(bool enabled)
	{
		masterButton->setChecked(enabled);
		masterButton->setText(enabled ? QStringLiteral("Sync: ON")
					      : QStringLiteral("Sync: OFF"));
	}

	QTableWidgetItem *cell(int row, int column)
	{
		QTableWidgetItem *item = table->item(row, column);
		if (!item) {
			item = new QTableWidgetItem();
			if (column != COL_SOURCE)
				item->setTextAlignment(Qt::AlignCenter);
			table->setItem(row, column, item);
		}
		return item;
	}

	void refresh()
	{
		struct irl_sync_config cfg;
		irl_sync_config_get(&cfg);

		refreshClock(cfg);
		refreshControls(cfg);
		refreshTable();
	}

	void refreshClock(const struct irl_sync_config &cfg)
	{
		struct irl_ntp_status ntp;
		irl_ntp_get_status(&ntp);

		int64_t utc_ns = 0;
		if (irl_ntp_utc_now_ns(&utc_ns)) {
			const qint64 ms = utc_ns / 1000000LL;
			clockLabel->setText(
				QDateTime::fromMSecsSinceEpoch(ms).toString(
					QStringLiteral("HH:mm:ss.zzz")));
			showingLabel->setText(
				QString("Showing %1  (offset −%2)")
					.arg(QDateTime::fromMSecsSinceEpoch(
						     ms - cfg.offset_ms)
						     .toString(QStringLiteral(
							     "HH:mm:ss.zzz")))
					.arg(format_ms(cfg.offset_ms)));
		} else {
			clockLabel->setText(QStringLiteral("--:--:--.---"));
			/* Not being synced is only a problem if sync is meant
			 * to be running. Polling is gated on the master switch,
			 * so before it is on there is nothing wrong to report. */
			showingLabel->setText(
				cfg.enabled
					? QStringLiteral(
						  "No clock reference — sync cannot run")
					: QStringLiteral(
						  "Turn sync on to start the clock reference"));
		}

		/* An unreachable server keeps the last offset ticking along
		 * plausibly, so freshness is reported, not just "synced". */
		QString health;
		bool fault = false;
		if (!ntp.server[0]) {
			health = QStringLiteral("no server configured");
			fault = cfg.enabled;
		} else if (!ntp.synced) {
			health = cfg.enabled
					 ? QStringLiteral("not synced")
					 : QStringLiteral("idle — polls while sync is on");
			fault = cfg.enabled;
		} else {
			health = QString("synced %1s ago  ±%2 ms")
					 .arg(ntp.age_ns / 1000000000ULL)
					 .arg(ntp.rtt_ns / 2000000LL);
			/* A stale reference keeps ticking plausibly while
			 * being wrong, so it reads as a fault too. */
			fault = cfg.enabled && ntp.age_ns > NTP_STALE_AGE_NS;
		}

		ntpLabel->setText(QString("NTP  %1  —  %2")
					  .arg(QString::fromUtf8(ntp.server))
					  .arg(health));
		ntpLabel->setStyleSheet(
			fault ? QStringLiteral("color: #c0392b;") : QString());
	}

	void refreshControls(const struct irl_sync_config &cfg)
	{
		updateMasterButton(cfg.enabled);

		/* Never fight the user mid-edit. */
		if (!offsetSpin->hasFocus() &&
		    offsetSpin->value() != cfg.offset_ms) {
			offsetSpin->blockSignals(true);
			offsetSpin->setValue(cfg.offset_ms);
			offsetSpin->blockSignals(false);
		}
		if (!ntpEdit->hasFocus() &&
		    ntpEdit->text() != QString::fromUtf8(cfg.ntp_server)) {
			ntpEdit->blockSignals(true);
			ntpEdit->setText(QString::fromUtf8(cfg.ntp_server));
			ntpEdit->blockSignals(false);
		}
	}

	void refreshTable()
	{
		struct irl_sync_entry entries[IRL_SYNC_MAX_SOURCES];
		const size_t count = irl_sync_collect(
			entries, sizeof(entries) / sizeof(entries[0]));

		/* Toggles every ~600ms: legible as a warning, slow enough not
		 * to read as a strobe. */
		if (++blinkTicks >= 3) {
			blinkTicks = 0;
			blinkOn = !blinkOn;
		}

		if (table->rowCount() != static_cast<int>(count))
			table->setRowCount(static_cast<int>(count));

		int alarms = 0;
		recommendedMs = 0;

		for (size_t i = 0; i < count; i++) {
			const struct irl_sync_entry &e = entries[i];
			const struct irl_sync_snapshot &s = e.snap;
			const int row = static_cast<int>(i);

			cell(row, COL_SOURCE)
				->setText(QString::fromUtf8(e.source_name));
			cell(row, COL_SYNC)
				->setText(e.sync_enabled
						  ? QStringLiteral("☑")
						  : QStringLiteral("☐"));
			cell(row, COL_TIMECODE)->setText(format_timecode(s));

			const bool measured = s.have_timecode &&
					      s.status != IRL_SYNC_OFF;
			cell(row, COL_LATENCY)
				->setText(measured ? format_ms(s.latency_ms)
						   : QStringLiteral("—"));
			cell(row, COL_PEAK)
				->setText(measured
						  ? format_ms(s.latency_peak_ms)
						  : QStringLiteral("—"));
			cell(row, COL_ADDED)
				->setText(s.status == IRL_SYNC_LOCKED ||
						  s.status == IRL_SYNC_ACQUIRING
						  ? format_ms(s.added_ms)
						  : QStringLiteral("—"));
			cell(row, COL_ERROR)
				->setText(s.status == IRL_SYNC_LOCKED ||
						  s.status == IRL_SYNC_ACQUIRING
						  ? format_ms(s.error_ms)
						  : QStringLiteral("—"));

			QTableWidgetItem *status = cell(row, COL_STATUS);
			status->setText(status_text(s));
			status->setForeground(status_colour(s.status));

			const bool alarm = s.status == IRL_SYNC_TOO_SLOW ||
					   s.status == IRL_SYNC_STALE;
			if (alarm) {
				alarms++;
				status->setBackground(
					blinkOn ? QBrush(QColor(192, 57, 43,
								90))
						: QBrush(Qt::NoBrush));
			} else {
				status->setBackground(QBrush(Qt::NoBrush));
			}

			if (e.sync_enabled &&
			    s.required_offset_ms > recommendedMs)
				recommendedMs = s.required_offset_ms;
		}

		recommendedLabel->setText(
			recommendedMs > 0
				? QString("(needs ≥ %1)")
					  .arg(format_ms(recommendedMs))
				: QString());
		autoButton->setEnabled(recommendedMs > 0);

		/* Carries the state when the dock is collapsed or narrow. */
		if (alarms > 0) {
			summaryLabel->setText(
				QString("%1 source%2 out of sync")
					.arg(alarms)
					.arg(alarms == 1
						     ? QString()
						     : QStringLiteral("s")));
			summaryLabel->setStyleSheet(
				QStringLiteral("color: #c0392b; font-weight: bold;"));
			summaryLabel->show();
		} else {
			summaryLabel->hide();
		}
	}
};

} // namespace

/* ── Registration ─────────────────────────────────────────── */

static bool dock_added = false;

void irl_sync_dock_register(void)
{
	if (dock_added)
		return;

	auto add_dock = reinterpret_cast<add_dock_by_id_fn>(
		resolve_frontend_symbol("obs_frontend_add_dock_by_id"));
	if (!add_dock) {
		blog(LOG_INFO,
		     "[irl-source] OBS frontend not available; sync dock disabled (status is still on the websocket vendor)");
		return;
	}

	/* Ownership passes to the frontend, which parents it into a
	 * QDockWidget and destroys it with the main window. */
	auto *dock = new SyncDock();
	dock->setWindowTitle(QStringLiteral("IRL Sync"));

	if (add_dock(IRL_SYNC_DOCK_ID, "IRL Sync", dock)) {
		dock_added = true;
		blog(LOG_INFO, "[irl-source] Sync dock registered");
	} else {
		delete dock;
		blog(LOG_WARNING, "[irl-source] Failed to register sync dock");
	}
}

void irl_sync_dock_unregister(void)
{
	if (!dock_added)
		return;

	/* Safe at shutdown as well as at a live unload: the frontend API
	 * checks its own callback table first and returns without doing
	 * anything once the main window is gone — which is also when it has
	 * already destroyed the widget and its refresh timer. */
	auto remove_dock = reinterpret_cast<remove_dock_fn>(
		resolve_frontend_symbol("obs_frontend_remove_dock"));
	if (remove_dock)
		remove_dock(IRL_SYNC_DOCK_ID);

	dock_added = false;
}

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
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
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

/* The clock is redrawn every tick so the milliseconds read as genuinely live
 * rather than stepping; the table and the alarm blink are decimated from it,
 * since neither benefits from more than a few updates a second. */
static constexpr int TICK_MS = 50;
static constexpr int TABLE_EVERY = 4;  /* ~5Hz */
static constexpr int BLINK_EVERY = 12; /* toggles every ~600ms */

/* Panel palette. Fixed rather than theme-derived on purpose: the clock is a
 * self-contained readout that paints its own dark ground, so it looks the same
 * under every OBS theme — the way a hardware timecode display does. */
static constexpr const char *PANEL_BG = "#0d1117";
static constexpr const char *PANEL_BORDER = "#30363d";
static constexpr const char *PANEL_DIM = "#7d8590";
static constexpr const char *PANEL_LIVE = "#3fb950";
static constexpr const char *PANEL_LIVE_DIM = "#2ea043";
static constexpr const char *PANEL_DEAD = "#484f58";

/* Points above the UI font, for both rows of digits. Smaller than the single
 * big clock it replaced, since there are two of them now and the dock shares
 * its column with the source table. */
static constexpr int DIGIT_POINTS = 15;

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

/* Every number in this dock changes in place, so all of them are drawn in the
 * fixed-pitch face. QFont::setStyleHint(QFont::Monospace) is only a hint and
 * leaves the proportional UI family in place, which makes digits jump
 * sideways as they tick — asking QFontDatabase for the real fixed font is what
 * actually holds them still. */
static QFont fixedFont(const QWidget *w, int pointDelta = 0, bool bold = false)
{
	QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	f.setPointSize(w->font().pointSize() + pointDelta);
	f.setBold(bold);
	return f;
}

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
		return QColor(63, 185, 80);
	case IRL_SYNC_ACQUIRING:
		return QColor(210, 153, 34);
	case IRL_SYNC_TOO_SLOW:
		return QColor(248, 81, 73);
	case IRL_SYNC_STALE:
		return QColor(191, 60, 53);
	case IRL_SYNC_NO_TIMECODE:
	case IRL_SYNC_OFF:
	default:
		return QColor(125, 133, 144);
	}
}

/* "No timecode" alone sends people looking in the wrong place, so each cause
 * carries its own fix. See irl_sync_tc_reason. */
static QString no_timecode_text(enum irl_sync_tc_reason reason)
{
	switch (reason) {
	case IRL_SYNC_TC_NO_CLOCK:
		return QStringLiteral("No timecode · no NTP reference here");
	case IRL_SYNC_TC_CODEC:
		return QStringLiteral("No timecode · stream is not H.265");
	case IRL_SYNC_TC_ABSENT:
		return QStringLiteral("No timecode · sender is not stamping");
	case IRL_SYNC_TC_OK:
	default:
		return QStringLiteral("No timecode");
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
		return QString("▲ Too slow · needs ≥ %1")
			.arg(format_ms(snap.required_offset_ms));
	case IRL_SYNC_STALE:
		return QStringLiteral("▲ No data");
	case IRL_SYNC_NO_TIMECODE:
		return QStringLiteral("○ ") + no_timecode_text(snap.tc_reason);
	case IRL_SYNC_OFF:
	default:
		return QStringLiteral("○ Off");
	}
}

/* Spelled out in full where there is room for it. */
static QString status_tooltip(const struct irl_sync_snapshot &snap)
{
	switch (snap.status) {
	case IRL_SYNC_NO_TIMECODE:
		switch (snap.tc_reason) {
		case IRL_SYNC_TC_NO_CLOCK:
			return QStringLiteral(
				"This machine has no NTP reference yet, so timecodes "
				"cannot be placed on a shared clock. Check the NTP "
				"server above.");
		case IRL_SYNC_TC_CODEC:
			return QStringLiteral(
				"SEI timecodes only exist on H.265/HEVC. Switch the "
				"sender's codec — Moblin's H.264 path does not write "
				"them at all.");
		case IRL_SYNC_TC_ABSENT:
			return QStringLiteral(
				"The stream is H.265 but carries no time_code SEI. On "
				"Moblin: Settings > Streams > (stream) > Video > "
				"Timecodes, and set an NTP pool. Timecodes only reach "
				"the wire over SRT, SRTLA or RIST — never RTMP.");
		default:
			break;
		}
		return QString();
	case IRL_SYNC_TOO_SLOW:
		return QStringLiteral(
			"This feed arrives later than the target offset, so its frames "
			"are already past their slot. Raise the offset, or improve that "
			"uplink.");
	case IRL_SYNC_STALE:
		return QStringLiteral("This feed has stopped delivering.");
	default:
		return QString();
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

		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this,
				 [this]() { tick(); });
		timer->start(TICK_MS);
	}

private:
	QLabel *clockLabel = nullptr;
	QLabel *showingCaption = nullptr;
	QLabel *showingLabel = nullptr;
	QLabel *ntpLabel = nullptr;
	QLabel *recommendedLabel = nullptr;
	QLabel *summaryLabel = nullptr;
	QSpinBox *offsetSpin = nullptr;
	QLineEdit *ntpEdit = nullptr;
	QPushButton *masterButton = nullptr;
	QPushButton *autoButton = nullptr;
	QTableWidget *table = nullptr;

	int ticks = 0;
	bool blinkOn = false;
	int64_t recommendedMs = 0;

	void build()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(12, 12, 12, 12);
		root->setSpacing(10);

		root->addWidget(buildClockPanel());
		root->addLayout(buildControls());

		summaryLabel = new QLabel(this);
		summaryLabel->setAlignment(Qt::AlignCenter);
		summaryLabel->hide();
		root->addWidget(summaryLabel);

		buildTable();
		root->addWidget(table, 1);
	}

	QWidget *buildClockPanel()
	{
		auto *panel = new QFrame(this);
		panel->setObjectName(QStringLiteral("irlClockPanel"));
		panel->setStyleSheet(
			QString("#irlClockPanel { background-color: %1;"
				" border: 1px solid %2; border-radius: 8px; }")
				.arg(PANEL_BG, PANEL_BORDER));

		auto *box = new QVBoxLayout(panel);
		box->setContentsMargins(16, 10, 16, 12);
		box->setSpacing(2);

		auto *caption = new QLabel(QStringLiteral("N T P   T I M E"),
					   panel);
		caption->setAlignment(Qt::AlignCenter);
		QFont capFont = fixedFont(this, -2, true);
		caption->setFont(capFont);
		caption->setStyleSheet(QString("color: %1;").arg(PANEL_DIM));
		box->addWidget(caption);

		clockLabel = new QLabel(QStringLiteral("--:--:--.---"), panel);
		clockLabel->setAlignment(Qt::AlignCenter);
		clockLabel->setFont(fixedFont(this, DIGIT_POINTS, true));
		/* Reserve the height the digits will need so the panel does not
		 * resize the moment the clock goes from placeholder to live. */
		clockLabel->setMinimumHeight(
			QFontMetrics(clockLabel->font()).height() + 4);
		box->addWidget(clockLabel);

		/* Carries the offset, because that is what the difference
		 * between the two rows of digits is. */
		showingCaption = new QLabel(panel);
		showingCaption->setAlignment(Qt::AlignCenter);
		showingCaption->setFont(capFont);
		showingCaption->setStyleSheet(
			QString("color: %1;").arg(PANEL_DIM));
		box->addWidget(showingCaption);

		/* Without this row the big clock and the source timecodes
		 * differ by exactly the offset and look like a bug. Same face
		 * and size as the row above: the pair is meant to read as one
		 * two-line display, not as a heading with a footnote. */
		showingLabel = new QLabel(QStringLiteral("--:--:--.---"), panel);
		showingLabel->setAlignment(Qt::AlignCenter);
		showingLabel->setFont(fixedFont(this, DIGIT_POINTS, true));
		showingLabel->setMinimumHeight(
			QFontMetrics(showingLabel->font()).height() + 4);
		box->addWidget(showingLabel);

		ntpLabel = new QLabel(panel);
		ntpLabel->setAlignment(Qt::AlignCenter);
		ntpLabel->setFont(fixedFont(this, -1));
		box->addWidget(ntpLabel);

		return panel;
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
		offsetSpin->setFont(fixedFont(this));
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
		masterButton->setMinimumWidth(90);
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
		table->verticalHeader()->setDefaultSectionSize(
			QFontMetrics(font()).height() + 12);
		table->setSelectionMode(QAbstractItemView::NoSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->setFocusPolicy(Qt::NoFocus);
		table->setAlternatingRowColors(true);
		/* Row striping carries the eye across; grid lines on top of it
		 * just add noise at this column count. */
		table->setShowGrid(false);
		table->horizontalHeader()->setHighlightSections(false);

		QFont headerFont = table->horizontalHeader()->font();
		headerFont.setBold(true);
		table->horizontalHeader()->setFont(headerFont);

		table->horizontalHeader()->setSectionResizeMode(
			COL_SOURCE, QHeaderView::Stretch);
		for (int i = COL_SYNC; i < COL_STATUS; i++) {
			table->horizontalHeader()->setSectionResizeMode(
				i, QHeaderView::ResizeToContents);
		}
		/* Status carries the longest, most important string, so it gets
		 * room of its own rather than being squeezed by the numbers. */
		table->horizontalHeader()->setSectionResizeMode(
			COL_STATUS, QHeaderView::Interactive);
		table->setColumnWidth(COL_STATUS, 240);
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
			if (column == COL_SOURCE)
				item->setTextAlignment(Qt::AlignLeft |
						       Qt::AlignVCenter);
			else if (column == COL_STATUS)
				item->setTextAlignment(Qt::AlignLeft |
						       Qt::AlignVCenter);
			else
				item->setTextAlignment(Qt::AlignCenter);

			/* The measured columns tick in place; the fixed face
			 * keeps them from shifting under the header. */
			if (column >= COL_TIMECODE && column <= COL_ERROR)
				item->setFont(fixedFont(this));
			table->setItem(row, column, item);
		}
		return item;
	}

	void tick()
	{
		struct irl_sync_config cfg;
		irl_sync_config_get(&cfg);

		refreshClock(cfg);

		if (++ticks % BLINK_EVERY == 0)
			blinkOn = !blinkOn;
		if (ticks % TABLE_EVERY == 0) {
			refreshControls(cfg);
			refreshTable();
		}
	}

	void refreshClock(const struct irl_sync_config &cfg)
	{
		struct irl_ntp_status ntp;
		irl_ntp_get_status(&ntp);

		int64_t utc_ns = 0;
		const bool live = irl_ntp_utc_now_ns(&utc_ns);

		if (live) {
			const qint64 ms = utc_ns / 1000000LL;
			clockLabel->setText(
				QDateTime::fromMSecsSinceEpoch(ms).toString(
					QStringLiteral("HH:mm:ss.zzz")));
			showingLabel->setText(
				QDateTime::fromMSecsSinceEpoch(ms -
							       cfg.offset_ms)
					.toString(QStringLiteral(
						"HH:mm:ss.zzz")));
			showingCaption->setText(
				QString("S H O W I N G   ·   O F F S E T   −%1")
					.arg(format_ms(cfg.offset_ms)));
		} else {
			clockLabel->setText(QStringLiteral("--:--:--.---"));
			showingLabel->setText(QStringLiteral("--:--:--.---"));
			/* Not being synced is only a problem if sync is meant
			 * to be running. Polling is gated on the master switch,
			 * so before it is on there is nothing wrong to report. */
			showingCaption->setText(
				cfg.enabled
					? QStringLiteral(
						  "NO CLOCK REFERENCE — SYNC CANNOT RUN")
					: QStringLiteral(
						  "TURN SYNC ON TO START THE CLOCK"));
		}
		clockLabel->setStyleSheet(
			QString("color: %1; letter-spacing: 2px;")
				.arg(live ? PANEL_LIVE : PANEL_DEAD));
		/* Dimmer than the row above so the reference clock still reads
		 * as the primary one. */
		showingLabel->setStyleSheet(
			QString("color: %1; letter-spacing: 2px;")
				.arg(live ? PANEL_LIVE_DIM : PANEL_DEAD));

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

		ntpLabel->setText(QString("%1  ·  %2")
					  .arg(QString::fromUtf8(ntp.server))
					  .arg(health));
		ntpLabel->setStyleSheet(
			QString("color: %1;")
				.arg(fault ? "#f85149" : PANEL_DIM));
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
			const bool aligning = s.status == IRL_SYNC_LOCKED ||
					      s.status == IRL_SYNC_ACQUIRING;
			cell(row, COL_LATENCY)
				->setText(measured ? format_ms(s.latency_ms)
						   : QStringLiteral("—"));
			cell(row, COL_PEAK)
				->setText(measured
						  ? format_ms(s.latency_peak_ms)
						  : QStringLiteral("—"));
			cell(row, COL_ADDED)
				->setText(aligning ? format_ms(s.added_ms)
						   : QStringLiteral("—"));
			cell(row, COL_ERROR)
				->setText(aligning ? format_ms(s.error_ms)
						   : QStringLiteral("—"));

			QTableWidgetItem *status = cell(row, COL_STATUS);
			status->setText(status_text(s));
			status->setForeground(status_colour(s.status));
			const QString tip = status_tooltip(s);
			status->setToolTip(tip);
			cell(row, COL_SOURCE)->setToolTip(tip);

			const bool alarm = s.status == IRL_SYNC_TOO_SLOW ||
					   s.status == IRL_SYNC_STALE;
			if (alarm) {
				alarms++;
				status->setBackground(
					blinkOn ? QBrush(QColor(248, 81, 73,
								70))
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
			summaryLabel->setStyleSheet(QStringLiteral(
				"color: #f85149; font-weight: bold;"));
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

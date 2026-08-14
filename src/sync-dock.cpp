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
 * Three rules shape the layout, each learned from watching it run:
 *
 *   Nothing moves when a value changes. Every figure is drawn in the
 *   fixed-pitch face, padded to a constant width, right-aligned, and sits in a
 *   column of fixed width. A dock whose columns twitch on each update cannot
 *   be read at a glance, which is the only way anyone reads it mid-show.
 *
 *   Off does not look like on. When sync is disabled the clock freezes and
 *   goes grey and the blink stops — stopped motion says "not running" faster
 *   than any label can.
 *
 *   One word per status. The detail that would widen a cell lives in the
 *   tooltip and the banner instead, where its length costs nothing.
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
#include <QGridLayout>
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

/* Fixed rather than theme-derived: the readout paints its own dark ground, so
 * it looks the same under every OBS theme — the way a rack-mounted timecode
 * display does. */
static constexpr const char *C_PANEL = "#0f1115";
static constexpr const char *C_INSET = "#0a0c10";
static constexpr const char *C_EDGE = "#7d6222";
static constexpr const char *C_AMBER = "#e0a54a";
static constexpr const char *C_GREEN = "#35e07a";
static constexpr const char *C_GREEN_DIM = "#249a54";
static constexpr const char *C_DIM = "#7d8590";
static constexpr const char *C_DEAD = "#454b54";
static constexpr const char *C_RED = "#e5534b";

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

/* ── Fonts and formatting ─────────────────────────────────── */

/* QFont::setStyleHint(QFont::Monospace) is only a hint and leaves the
 * proportional UI family in place, which makes digits jump sideways as they
 * tick. Asking QFontDatabase for the real fixed font is what holds them
 * still. */
static QFont fixedFont(const QWidget *w, int pointDelta = 0, bool bold = false)
{
	QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	f.setPointSize(w->font().pointSize() + pointDelta);
	f.setBold(bold);
	return f;
}

static QFont captionFont(const QWidget *w, int pointDelta = -2)
{
	QFont f = w->font();
	f.setPointSize(f.pointSize() + pointDelta);
	f.setBold(true);
	f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
	return f;
}

/* Padding is what keeps the table still: every figure occupies the same number
 * of characters whatever its magnitude, so in a fixed-pitch face at a fixed
 * column width the digits cannot reflow. Sub-second values stay in
 * milliseconds for precision and larger ones switch to seconds; both are
 * padded to the width of the longest form either can produce. */
static constexpr int VALUE_CHARS = 9;

static QString pad(const QString &text)
{
	return text.rightJustified(VALUE_CHARS, QChar(' '));
}

static QString format_value(int64_t ms)
{
	if (ms > -1000 && ms < 1000)
		return pad(QString("%1 ms").arg(ms));
	return pad(QString("%1 s").arg(static_cast<double>(ms) / 1000.0, 0, 'f',
				       2));
}

static QString format_seconds(int64_t ms)
{
	return QString("%1 s").arg(static_cast<double>(ms) / 1000.0, 0, 'f', 2);
}

static QString format_timecode(const struct irl_sync_snapshot &snap)
{
	if (!snap.have_timecode)
		return QStringLiteral("--:--:--:--");

	return QString("%1:%2:%3:%4")
		.arg(snap.tc.hours, 2, 10, QChar('0'))
		.arg(snap.tc.minutes, 2, 10, QChar('0'))
		.arg(snap.tc.seconds, 2, 10, QChar('0'))
		.arg(snap.tc.n_frames, 2, 10, QChar('0'));
}

/* One word per state, so the column can be a fixed width and the eye can sort
 * the table at a glance. */
static QString status_label(enum irl_sync_status status)
{
	switch (status) {
	case IRL_SYNC_LOCKED:
		return QStringLiteral("LOCKED");
	case IRL_SYNC_ACQUIRING:
		return QStringLiteral("ACQUIRING");
	case IRL_SYNC_TOO_SLOW:
		return QStringLiteral("TOO SLOW");
	case IRL_SYNC_STALE:
		return QStringLiteral("NO DATA");
	case IRL_SYNC_NO_TIMECODE:
		return QStringLiteral("NO TIMECODE");
	case IRL_SYNC_OFF:
	default:
		return QStringLiteral("OFF");
	}
}

static QColor status_colour(enum irl_sync_status status)
{
	switch (status) {
	case IRL_SYNC_LOCKED:
		return QColor(53, 224, 122);
	case IRL_SYNC_ACQUIRING:
		return QColor(224, 165, 74);
	case IRL_SYNC_TOO_SLOW:
	case IRL_SYNC_STALE:
		return QColor(229, 83, 75);
	case IRL_SYNC_NO_TIMECODE:
	case IRL_SYNC_OFF:
	default:
		return QColor(125, 133, 144);
	}
}

/* Each cause of "no timecode" has a different fix, so the full explanation
 * stays one hover away even though the cell shows a single word. */
static QString status_tooltip(const struct irl_sync_snapshot &snap)
{
	switch (snap.status) {
	case IRL_SYNC_NO_TIMECODE:
		switch (snap.tc_reason) {
		case IRL_SYNC_TC_NO_CLOCK:
			return QStringLiteral(
				"No NTP reference on this machine yet, so timecodes "
				"cannot be placed on a shared clock.");
		case IRL_SYNC_TC_CODEC:
			return QStringLiteral(
				"SEI timecodes only exist on H.265/HEVC. Switch the "
				"sender's codec — Moblin's H.264 path does not write "
				"them at all.");
		case IRL_SYNC_TC_ABSENT:
			return QStringLiteral(
				"The stream is H.265 but carries no time_code SEI. On "
				"Moblin: Settings > Streams > (stream) > Video > "
				"Timecodes, with an NTP pool set on the screen behind "
				"it. The stream must be stopped to change either, and "
				"timecodes only reach the wire over SRT, SRTLA or "
				"RIST — never RTMP.");
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
	QLabel *badge = nullptr;
	QLabel *sourceLabel = nullptr;
	QLabel *clockLabel = nullptr;
	QLabel *clockMsLabel = nullptr;
	QLabel *showingLabel = nullptr;
	QLabel *offsetBoxValue = nullptr;
	QLabel *syncedLabel = nullptr;
	QLabel *driftLabel = nullptr;
	QLabel *minimumLabel = nullptr;
	QLabel *banner = nullptr;
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
		setStyleSheet(QString("QWidget { background-color: %1;"
				      " color: #c9d1d9; }")
				      .arg(C_PANEL));

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(12, 10, 12, 12);
		root->setSpacing(10);

		root->addLayout(buildHeader());
		root->addWidget(buildClockPanel());
		root->addLayout(buildControls());
		root->addWidget(buildBanner());
		root->addWidget(buildTable(), 1);
	}

	QHBoxLayout *buildHeader()
	{
		auto *row = new QHBoxLayout();
		row->setSpacing(8);

		auto *icon = new QLabel(QStringLiteral("◷"), this);
		QFont iconFont = font();
		iconFont.setPointSize(iconFont.pointSize() + 4);
		icon->setFont(iconFont);
		icon->setStyleSheet(QString("color: %1;").arg(C_AMBER));
		row->addWidget(icon);

		auto *title = new QLabel(QStringLiteral("IRL SYNC"), this);
		title->setFont(captionFont(this, 1));
		row->addWidget(title);

		row->addStretch(1);

		/* The state, said once, where the eye lands first. */
		badge = new QLabel(this);
		badge->setFont(captionFont(this, -1));
		badge->setAlignment(Qt::AlignCenter);
		row->addWidget(badge);

		return row;
	}

	QWidget *buildClockPanel()
	{
		auto *panel = new QFrame(this);
		panel->setObjectName(QStringLiteral("irlClockPanel"));
		panel->setStyleSheet(
			QString("#irlClockPanel { background-color: %1;"
				" border: 1px solid %2; border-radius: 4px; }")
				.arg(C_INSET, C_EDGE));

		auto *grid = new QGridLayout(panel);
		grid->setContentsMargins(14, 10, 14, 12);
		grid->setHorizontalSpacing(12);
		grid->setVerticalSpacing(2);

		sourceLabel = new QLabel(this);
		sourceLabel->setFont(captionFont(this));
		sourceLabel->setStyleSheet(QString("color: %1;").arg(C_AMBER));
		grid->addWidget(sourceLabel, 0, 0);

		/* Split so the milliseconds can be small and dim without
		 * dragging the seconds around: two labels side by side each
		 * keep their own advance width. */
		auto *clockRow = new QHBoxLayout();
		clockRow->setSpacing(0);
		clockLabel = new QLabel(QStringLiteral("--:--:--"), this);
		clockLabel->setFont(fixedFont(this, 18, true));
		clockRow->addWidget(clockLabel);
		clockMsLabel = new QLabel(QStringLiteral(".---"), this);
		clockMsLabel->setFont(fixedFont(this, 5, true));
		clockRow->addWidget(clockMsLabel, 0, Qt::AlignBottom);
		clockRow->addStretch(1);
		grid->addLayout(clockRow, 1, 0);

		auto *showingCaption =
			new QLabel(QStringLiteral("SHOWING"), this);
		showingCaption->setFont(captionFont(this));
		showingCaption->setStyleSheet(
			QString("color: %1;").arg(C_AMBER));
		grid->addWidget(showingCaption, 2, 0);

		/* Without this the big clock and the source timecodes differ by
		 * exactly the offset and look like a bug. */
		showingLabel = new QLabel(QStringLiteral("--:--:--.---"), this);
		showingLabel->setFont(fixedFont(this, 4, true));
		grid->addWidget(showingLabel, 3, 0);

		grid->addWidget(buildOffsetBox(), 0, 1, 2, 1,
				Qt::AlignRight | Qt::AlignTop);

		syncedLabel = new QLabel(this);
		syncedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		grid->addWidget(syncedLabel, 2, 1);

		driftLabel = new QLabel(this);
		driftLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		driftLabel->setStyleSheet(QString("color: %1;").arg(C_DIM));
		grid->addWidget(driftLabel, 3, 1);

		grid->setColumnStretch(0, 1);

		return panel;
	}

	QWidget *buildOffsetBox()
	{
		auto *box = new QFrame(this);
		box->setObjectName(QStringLiteral("irlOffsetBox"));
		box->setStyleSheet(
			QString("#irlOffsetBox { background-color: %1;"
				" border: 1px solid %2; border-radius: 3px; }")
				.arg(C_INSET, C_EDGE));

		auto *col = new QVBoxLayout(box);
		col->setContentsMargins(12, 4, 12, 6);
		col->setSpacing(0);

		auto *caption = new QLabel(QStringLiteral("OFFSET"), this);
		caption->setFont(captionFont(this, -1));
		caption->setStyleSheet(QString("color: %1;").arg(C_AMBER));
		caption->setAlignment(Qt::AlignCenter);
		col->addWidget(caption);

		offsetBoxValue = new QLabel(this);
		offsetBoxValue->setFont(fixedFont(this, 5, true));
		offsetBoxValue->setStyleSheet(
			QString("color: %1;").arg(C_AMBER));
		offsetBoxValue->setAlignment(Qt::AlignCenter);
		col->addWidget(offsetBoxValue);

		return box;
	}

	QVBoxLayout *labelledColumn(const QString &caption, QWidget *field)
	{
		auto *col = new QVBoxLayout();
		col->setSpacing(3);
		auto *label = new QLabel(caption, this);
		label->setFont(captionFont(this));
		label->setStyleSheet(QString("color: %1;").arg(C_DIM));
		col->addWidget(label);
		col->addWidget(field);
		return col;
	}

	QHBoxLayout *buildControls()
	{
		auto *row = new QHBoxLayout();
		row->setSpacing(8);

		offsetSpin = new QSpinBox(this);
		offsetSpin->setRange(IRL_SYNC_MIN_OFFSET_MS,
				     IRL_SYNC_MAX_OFFSET_MS);
		offsetSpin->setSingleStep(250);
		offsetSpin->setSuffix(QStringLiteral(" ms"));
		offsetSpin->setValue(irl_sync_offset_ms());
		offsetSpin->setFont(fixedFont(this));
		offsetSpin->setFixedWidth(120);
		/* Named on the declaring class: editingFinished and clicked are
		 * inherited, and spelling the base out keeps the
		 * pointer-to-member unambiguous. */
		QObject::connect(offsetSpin,
				 &QAbstractSpinBox::editingFinished, this,
				 [this]() {
					 irl_sync_set_offset_ms(
						 offsetSpin->value());
				 });
		row->addLayout(labelledColumn(QStringLiteral("OFFSET"),
					      offsetSpin));

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
		row->addLayout(labelledColumn(QStringLiteral("NTP SERVER"),
					      ntpEdit),
			       1);

		autoButton = new QPushButton(QStringLiteral("Auto"), this);
		autoButton->setToolTip(QStringLiteral(
			"Set the offset from the worst synced feed's peak "
			"arrival latency, plus margin."));
		autoButton->setFixedWidth(64);
		QObject::connect(autoButton, &QAbstractButton::clicked, this,
				 [this]() {
					 if (recommendedMs > 0)
						 irl_sync_set_offset_ms(
							 static_cast<int>(
								 recommendedMs));
				 });
		row->addLayout(labelledColumn(QString(), autoButton));

		/* The button says what it will do, the badge says what is
		 * true — between them there is nothing to misread. */
		masterButton = new QPushButton(this);
		masterButton->setFixedWidth(112);
		QObject::connect(masterButton, &QAbstractButton::clicked, this,
				 [this]() {
					 irl_sync_set_enabled(
						 !irl_sync_is_enabled());
				 });
		row->addLayout(labelledColumn(QString(), masterButton));

		return row;
	}

	QWidget *buildBanner()
	{
		auto *column = new QWidget(this);
		auto *box = new QVBoxLayout(column);
		box->setContentsMargins(0, 0, 0, 0);
		box->setSpacing(6);

		minimumLabel = new QLabel(this);
		minimumLabel->setStyleSheet(QString("color: %1;").arg(C_DIM));
		box->addWidget(minimumLabel);

		banner = new QLabel(this);
		banner->setObjectName(QStringLiteral("irlBanner"));
		banner->setStyleSheet(
			QString("#irlBanner { background-color: #2a1416;"
				" border-left: 3px solid %1; color: %1;"
				" padding: 6px 10px; }")
				.arg(C_RED));
		banner->hide();
		box->addWidget(banner);

		return column;
	}

	QWidget *buildTable()
	{
		table = new QTableWidget(0, COL_COUNT, this);
		table->setHorizontalHeaderLabels(
			{QStringLiteral("SOURCE"), QStringLiteral("TIMECODE"),
			 QStringLiteral("LATENCY"), QStringLiteral("PEAK 60 S"),
			 QStringLiteral("ADDED"), QStringLiteral("ERROR"),
			 QStringLiteral("STATUS")});
		table->verticalHeader()->setVisible(false);
		table->verticalHeader()->setDefaultSectionSize(
			QFontMetrics(font()).height() + 14);
		table->setSelectionMode(QAbstractItemView::NoSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->setFocusPolicy(Qt::NoFocus);
		table->setShowGrid(false);
		table->setStyleSheet(
			QString("QTableWidget { background-color: %1;"
				" border: none; }"
				"QHeaderView::section { background-color: %1;"
				" color: %2; border: none;"
				" border-bottom: 1px solid #21262d;"
				" padding: 4px 6px; }")
				.arg(C_PANEL, C_DIM));

		QFont headerFont = captionFont(this);
		table->horizontalHeader()->setFont(headerFont);
		table->horizontalHeader()->setHighlightSections(false);
		table->horizontalHeader()->setStretchLastSection(false);

		/* Every measured column is pinned to the width of the widest
		 * string it can ever hold, so a value changing magnitude can
		 * never reflow the table. Only SOURCE flexes, and what it holds
		 * is a name that does not change while running. */
		QFontMetrics fm(fixedFont(this));
		const int valueWidth =
			fm.horizontalAdvance(QString(VALUE_CHARS, '0')) + 16;
		const int timecodeWidth =
			fm.horizontalAdvance(QStringLiteral("00:00:00:00")) + 20;
		QFontMetrics hfm(headerFont);
		const int statusWidth =
			hfm.horizontalAdvance(QStringLiteral("NO TIMECODE")) +
			24;

		auto *header = table->horizontalHeader();
		header->setSectionResizeMode(COL_SOURCE, QHeaderView::Stretch);
		for (int i = COL_TIMECODE; i < COL_COUNT; i++)
			header->setSectionResizeMode(i, QHeaderView::Fixed);
		table->setColumnWidth(COL_TIMECODE, timecodeWidth);
		table->setColumnWidth(COL_LATENCY, valueWidth);
		table->setColumnWidth(COL_PEAK, valueWidth);
		table->setColumnWidth(COL_ADDED, valueWidth);
		table->setColumnWidth(COL_ERROR, valueWidth);
		table->setColumnWidth(COL_STATUS, statusWidth);

		return table;
	}

	QTableWidgetItem *cell(int row, int column)
	{
		QTableWidgetItem *item = table->item(row, column);
		if (!item) {
			item = new QTableWidgetItem();
			if (column == COL_SOURCE) {
				item->setTextAlignment(Qt::AlignLeft |
						       Qt::AlignVCenter);
			} else if (column == COL_STATUS) {
				item->setTextAlignment(Qt::AlignCenter);
				item->setFont(captionFont(this));
			} else {
				/* Right-aligned and fixed-pitch: the last digit
				 * stays on the same pixel as values grow. */
				item->setTextAlignment(Qt::AlignRight |
						       Qt::AlignVCenter);
				item->setFont(fixedFont(this));
			}
			table->setItem(row, column, item);
		}
		return item;
	}

	void tick()
	{
		struct irl_sync_config cfg;
		irl_sync_config_get(&cfg);

		refreshClock(cfg);

		/* Motion is the tell that something is running, so it stops
		 * with the feature. */
		if (cfg.enabled) {
			if (ticks % BLINK_EVERY == 0)
				blinkOn = !blinkOn;
		} else {
			blinkOn = false;
		}

		if (ticks % TABLE_EVERY == 0) {
			refreshControls(cfg);
			refreshTable(cfg);
		}
		ticks++;
	}

	void refreshClock(const struct irl_sync_config &cfg)
	{
		struct irl_ntp_status ntp;
		irl_ntp_get_status(&ntp);

		int64_t utc_ns = 0;
		const bool live = cfg.enabled && irl_ntp_utc_now_ns(&utc_ns);

		if (live) {
			const qint64 ms = utc_ns / 1000000LL;
			const QDateTime now = QDateTime::fromMSecsSinceEpoch(ms);
			clockLabel->setText(
				now.toString(QStringLiteral("HH:mm:ss")));
			clockMsLabel->setText(
				now.toString(QStringLiteral(".zzz")));
			showingLabel->setText(
				QDateTime::fromMSecsSinceEpoch(ms -
							       cfg.offset_ms)
					.toString(QStringLiteral(
						"HH:mm:ss.zzz")));
		} else if (cfg.enabled) {
			clockLabel->setText(QStringLiteral("--:--:--"));
			clockMsLabel->setText(QStringLiteral(".---"));
			showingLabel->setText(QStringLiteral("--:--:--.---"));
		}
		/* Sync off leaves the digits exactly where they stopped. A
		 * frozen readout reads as "paused" at a glance, where dashes
		 * read as "broken". */

		clockLabel->setStyleSheet(
			QString("color: %1;").arg(live ? C_GREEN : C_DEAD));
		clockMsLabel->setStyleSheet(
			QString("color: %1;").arg(live ? C_GREEN_DIM : C_DEAD));
		showingLabel->setStyleSheet(
			QString("color: %1;").arg(live ? C_GREEN_DIM : C_DEAD));

		sourceLabel->setText(
			QString("NTP TIME · %1")
				.arg(QString::fromUtf8(ntp.server).toUpper()));
		offsetBoxValue->setText(
			QString("−%1").arg(format_seconds(cfg.offset_ms)));

		/* An unreachable server keeps the last offset ticking along
		 * plausibly, so freshness is reported, not just "synced". */
		QString synced;
		const char *syncedColour = C_DIM;
		if (!cfg.enabled) {
			synced = QStringLiteral("Idle");
		} else if (!ntp.server[0]) {
			synced = QStringLiteral("No server");
			syncedColour = C_RED;
		} else if (!ntp.synced) {
			synced = QStringLiteral("Not synced");
			syncedColour = C_RED;
		} else {
			synced = QString("● Synced %1 s ago")
					 .arg(ntp.age_ns / 1000000000ULL);
			/* A stale reference keeps ticking plausibly while being
			 * wrong, so it reads as a fault too. */
			syncedColour = ntp.age_ns > NTP_STALE_AGE_NS ? C_RED
								     : C_GREEN;
		}
		syncedLabel->setText(synced);
		syncedLabel->setStyleSheet(
			QString("color: %1;").arg(syncedColour));
		driftLabel->setText(
			cfg.enabled && ntp.synced
				? QString("±%1 ms drift")
					  .arg(ntp.rtt_ns / 2000000LL)
				: QString());
	}

	void refreshControls(const struct irl_sync_config &cfg)
	{
		badge->setText(cfg.enabled ? QStringLiteral(" SYNC ON ")
					   : QStringLiteral(" SYNC OFF "));
		badge->setStyleSheet(
			QString("color: %1; border: 1px solid %1;"
				" border-radius: 3px; padding: 2px 6px;")
				.arg(cfg.enabled ? C_GREEN : C_RED));

		masterButton->setText(cfg.enabled
					      ? QStringLiteral("Disable sync")
					      : QStringLiteral("Enable sync"));
		masterButton->setStyleSheet(
			QString("QPushButton { color: %1;"
				" border: 1px solid %1; border-radius: 3px;"
				" padding: 4px; }")
				.arg(cfg.enabled ? C_DIM : C_RED));

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

	void refreshTable(const struct irl_sync_config &cfg)
	{
		struct irl_sync_entry entries[IRL_SYNC_MAX_SOURCES];
		const size_t count = irl_sync_collect(
			entries, sizeof(entries) / sizeof(entries[0]));

		if (table->rowCount() != static_cast<int>(count))
			table->setRowCount(static_cast<int>(count));

		int alarms = 0;
		bool belowMinimum = false;
		recommendedMs = 0;

		for (size_t i = 0; i < count; i++) {
			const struct irl_sync_entry &e = entries[i];
			const struct irl_sync_snapshot &s = e.snap;
			const int row = static_cast<int>(i);

			cell(row, COL_SOURCE)
				->setText(QString("%1  %2")
						  .arg(e.sync_enabled
							       ? QStringLiteral("☑")
							       : QStringLiteral("☐"))
						  .arg(QString::fromUtf8(
							  e.source_name)));

			QTableWidgetItem *tc = cell(row, COL_TIMECODE);
			tc->setText(format_timecode(s));
			tc->setForeground(s.have_timecode ? QColor(53, 224, 122)
							  : QColor(69, 75, 84));

			const bool measured =
				s.have_timecode && s.status != IRL_SYNC_OFF;
			const bool aligning = s.status == IRL_SYNC_LOCKED ||
					      s.status == IRL_SYNC_ACQUIRING;
			cell(row, COL_LATENCY)
				->setText(measured ? format_value(s.latency_ms)
						   : pad(QStringLiteral("—")));
			cell(row, COL_PEAK)
				->setText(measured ? format_value(
							     s.latency_peak_ms)
						   : pad(QStringLiteral("—")));
			cell(row, COL_ADDED)
				->setText(aligning ? format_value(s.added_ms)
						   : pad(QStringLiteral("—")));
			cell(row, COL_ERROR)
				->setText(aligning ? format_value(s.error_ms)
						   : pad(QStringLiteral("—")));

			QTableWidgetItem *status = cell(row, COL_STATUS);
			status->setText(status_label(s.status));
			status->setForeground(status_colour(s.status));
			const QString tip = status_tooltip(s);
			status->setToolTip(tip);
			cell(row, COL_SOURCE)->setToolTip(tip);

			const bool alarm = s.status == IRL_SYNC_TOO_SLOW ||
					   s.status == IRL_SYNC_STALE;
			if (alarm) {
				alarms++;
				if (s.status == IRL_SYNC_TOO_SLOW)
					belowMinimum = true;
				status->setBackground(
					blinkOn ? QBrush(QColor(229, 83, 75, 70))
						: QBrush(Qt::NoBrush));
			} else {
				status->setBackground(QBrush(Qt::NoBrush));
			}

			if (e.sync_enabled &&
			    s.required_offset_ms > recommendedMs)
				recommendedMs = s.required_offset_ms;
		}

		minimumLabel->setText(
			QString("Minimum offset for current sources: %1")
				.arg(recommendedMs > 0
					     ? format_seconds(recommendedMs)
					     : QStringLiteral("—")));
		autoButton->setEnabled(recommendedMs > 0 && cfg.enabled);

		/* Carries the state when the table is scrolled out of view. */
		if (alarms > 0 && cfg.enabled) {
			banner->setText(
				QString("⚠  %1 source%2 out of sync%3")
					.arg(alarms)
					.arg(alarms == 1 ? QString()
							 : QStringLiteral("s"))
					.arg(belowMinimum
						     ? QStringLiteral(
							       " — offset is below the required minimum")
						     : QString()));
			banner->show();
		} else {
			banner->hide();
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

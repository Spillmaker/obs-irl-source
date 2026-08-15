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
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSize>
#include <QSpinBox>
#include <QStyle>
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

/* Character counts every measured cell is padded out to, and every measured
 * column is sized from. Both are worst cases, not typical ones, so a value can
 * never widen its column and a column can never crop its value:
 *
 *   VALUE_CHARS  "-1800.00 s" — format_ms at the ±30 minute wrap that
 *                timecode_latency_ns folds arrival latency into.
 *   TC_CHARS     "HH:MM:SS:FFF" — n_frames is a 9-bit field, so three digits
 *                are legal even though 60fps senders never pass two.
 *
 * Padding as well as fixing the width matters: the cells are centred, so
 * without it a shorter string would still slide around inside a column that
 * itself was not moving. */
static constexpr int VALUE_CHARS = 10;
static constexpr int TC_CHARS = 12;

/* Slack added to a measured column so the text is not flush against the cell
 * edge, and so a theme with roomier cell margins still clears the value. */
static constexpr int CELL_PADDING_PX = 20;

static QString pad(const QString &text, int chars)
{
	return text.rightJustified(chars, QChar(' '));
}

static QString format_ms(int64_t ms)
{
	if (ms > -1000 && ms < 1000)
		return QString("%1 ms").arg(ms);
	return QString("%1 s").arg(static_cast<double>(ms) / 1000.0, 0, 'f', 2);
}

/* Padded variants, for the cells. The bare formatter stays for prose. */
static QString format_value(int64_t ms)
{
	return pad(format_ms(ms), VALUE_CHARS);
}

static QString no_value(int chars)
{
	return pad(QStringLiteral("—"), chars);
}

static QString format_timecode(const struct irl_sync_snapshot &snap)
{
	if (!snap.have_timecode)
		return no_value(TC_CHARS);

	return pad(QString("%1:%2:%3:%4")
			   .arg(snap.tc.hours, 2, 10, QChar('0'))
			   .arg(snap.tc.minutes, 2, 10, QChar('0'))
			   .arg(snap.tc.seconds, 2, 10, QChar('0'))
			   .arg(snap.tc.n_frames, 2, 10, QChar('0')),
		   TC_CHARS);
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
		return QString("▲ Too slow · needs ≥ %1 s")
			.arg(snap.required_offset_ms / 1000);
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

/* Widest string status_text can ever return, in pixels.
 *
 * Generated by running every state through the real formatter rather than
 * measuring a hardcoded specimen, so a reworded status cannot silently start
 * being cropped. */
static int widest_status_px(const QWidget *w)
{
	static const enum irl_sync_status statuses[] = {
		IRL_SYNC_OFF,      IRL_SYNC_NO_TIMECODE, IRL_SYNC_STALE,
		IRL_SYNC_TOO_SLOW, IRL_SYNC_ACQUIRING,   IRL_SYNC_LOCKED};
	static const enum irl_sync_tc_reason reasons[] = {
		IRL_SYNC_TC_OK, IRL_SYNC_TC_NO_CLOCK, IRL_SYNC_TC_CODEC,
		IRL_SYNC_TC_ABSENT};

	const QFontMetrics fm(w->font());
	struct irl_sync_snapshot probe = {};
	/* The one status that interpolates a number, at its largest. */
	probe.required_offset_ms = IRL_SYNC_MAX_OFFSET_MS;

	int widest = 0;
	for (enum irl_sync_status status : statuses) {
		probe.status = status;
		for (enum irl_sync_tc_reason reason : reasons) {
			probe.tc_reason = reason;
			widest = qMax(widest,
				      fm.horizontalAdvance(status_text(probe)));
		}
	}
	return widest;
}

/* Borrow OBS's own gear rather than shipping one. The frontend stylesheet
 * attaches the artwork to these property selectors, so a button that declares
 * them picks up whatever the running theme uses on the Controls dock — and
 * follows the user's theme for free. The property name changed with the theme
 * engine in OBS 30.2, so both spellings are set; a theme that honours neither
 * leaves the icon null and gets the glyph instead. */
static void applyGearIcon(QPushButton *button)
{
	button->setProperty("themeID", "configIconSmall");
	button->setProperty("class", "icon-gear");
	button->style()->unpolish(button);
	button->style()->polish(button);

	if (button->icon().isNull())
		button->setText(QStringLiteral("⚙"));
	else
		button->setIconSize(QSize(16, 16));

	button->setMaximumWidth(32);
}

/* ── Dock widget ──────────────────────────────────────────── */

namespace {

/* Only sources with Sync ticked are listed, so there is no column saying
 * whether they are: every row is one. */
enum column {
	COL_SOURCE = 0,
	COL_TIMECODE,
	COL_LATENCY,
	COL_ADDED,
	COL_DRIFT,
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
	QLabel *serverCaption = nullptr;
	QLabel *clockLabel = nullptr;
	QLabel *showingCaption = nullptr;
	QLabel *showingLabel = nullptr;
	QLabel *ntpLabel = nullptr;
	QLabel *summaryLabel = nullptr;
	QSpinBox *offsetSpin = nullptr;
	QPushButton *applyButton = nullptr;
	QPushButton *masterButton = nullptr;
	QTableWidget *table = nullptr;

	int ticks = 0;
	bool blinkOn = false;
	/* The offset the spinbox was last synced to, so a change made anywhere
	 * else can be told apart from one the user has typed but not applied —
	 * the first should be adopted, the second must not be overwritten. */
	int knownOffsetMs = 0;

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

		/* The reference this clock is disciplined to, named rather than
		 * labelled "NTP time": which server it is is the thing that has
		 * to match the senders, and the zone is spelled out because the
		 * timecodes below arrive stamped in UTC too. */
		serverCaption = new QLabel(panel);
		serverCaption->setAlignment(Qt::AlignCenter);
		QFont capFont = fixedFont(this, -2, true);
		serverCaption->setFont(capFont);
		serverCaption->setStyleSheet(
			QString("color: %1;").arg(PANEL_DIM));
		box->addWidget(serverCaption);

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

		/* Whole seconds. See IRL_SYNC_OFFSET_STEP_MS: this is the
		 * number co-streamers agree on out loud, so the control offers
		 * exactly the values that are worth saying. */
		offsetSpin = new QSpinBox(this);
		offsetSpin->setRange(IRL_SYNC_MIN_OFFSET_MS / 1000,
				     IRL_SYNC_MAX_OFFSET_MS / 1000);
		offsetSpin->setSuffix(QStringLiteral(" s"));
		offsetSpin->setValue(irl_sync_offset_ms() / 1000);
		offsetSpin->setFont(fixedFont(this));
		knownOffsetMs = irl_sync_offset_ms();
		QObject::connect(offsetSpin, &QSpinBox::valueChanged, this,
				 [this](int) { updateApplyButton(); });
		row->addWidget(offsetSpin);

		/* Explicit, rather than applying on editingFinished as this
		 * used to. Changing the offset is a several-second step for
		 * every synced feed at once, so it should happen when someone
		 * says so — not when they tab away, and not silently on a
		 * keypress they have to know about. */
		applyButton = new QPushButton(QStringLiteral("Update"), this);
		applyButton->setToolTip(QStringLiteral(
			"Apply the new offset to every synced source."));
		/* Named on the declaring class: clicked is inherited, and
		 * spelling the base out keeps the pointer-to-member
		 * unambiguous. */
		QObject::connect(applyButton, &QAbstractButton::clicked, this,
				 [this]() {
					 irl_sync_set_offset_ms(
						 offsetSpin->value() * 1000);
					 updateApplyButton();
				 });
		row->addWidget(applyButton);

		row->addStretch(1);

		auto *settingsButton = new QPushButton(this);
		settingsButton->setToolTip(QStringLiteral("Sync settings"));
		applyGearIcon(settingsButton);
		QObject::connect(settingsButton, &QAbstractButton::clicked, this,
				 [this]() { openSettings(); });
		row->addWidget(settingsButton);

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
			{QStringLiteral("Source"), QStringLiteral("Timecode"),
			 QStringLiteral("Latency"), QStringLiteral("Added"),
			 QStringLiteral("Drift"), QStringLiteral("Status")});
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

		/* Every measured column is nailed to the width of its widest
		 * possible value.
		 *
		 * ResizeToContents is what these used to be, and it is wrong
		 * here for a reason worth recording: the values tick several
		 * times a second, so the column tracked them, and because a
		 * column resize shifts every column after it, one digit
		 * appearing in Error moved the whole right-hand side of the
		 * table. A readout that jitters as it updates is harder to read
		 * than one that is a few pixels wider than it needs to be. */
		auto fix = [&](int column, int chars) {
			const QFontMetrics values(fixedFont(this));
			const QFontMetrics header(
				table->horizontalHeader()->font());
			/* The header is bold and in the UI face, so a short
			 * column can be titled wider than anything it holds. */
			const QTableWidgetItem *title =
				table->horizontalHeaderItem(column);
			const int width =
				qMax(values.horizontalAdvance(
					     QString(chars, QChar('0'))),
				     title ? header.horizontalAdvance(
						     title->text())
					   : 0) +
				CELL_PADDING_PX;
			table->horizontalHeader()->setSectionResizeMode(
				column, QHeaderView::Fixed);
			table->setColumnWidth(column, width);
		};

		fix(COL_TIMECODE, TC_CHARS);
		fix(COL_LATENCY, VALUE_CHARS);
		fix(COL_ADDED, VALUE_CHARS);
		fix(COL_DRIFT, VALUE_CHARS);

		/* Status is prose rather than a figure, so it is sized from the
		 * longest wording instead of a character count, and left
		 * resizable because it is the one column someone might want to
		 * reclaim space from. Interactive never resizes itself, so it
		 * does not reintroduce the jitter. */
		table->horizontalHeader()->setSectionResizeMode(
			COL_STATUS, QHeaderView::Interactive);
		table->setColumnWidth(COL_STATUS,
				      widest_status_px(this) + CELL_PADDING_PX);

		/* Last, so the stretch absorbs whatever the fixed columns
		 * leave rather than fighting them for it. */
		table->horizontalHeader()->setSectionResizeMode(
			COL_SOURCE, QHeaderView::Stretch);
	}

	/* Modal and apply-on-OK, unlike the offset next to it. What lives here
	 * is set once when the setup is built and then left alone, so it is
	 * worth a deliberate confirmation and not worth the live round trip the
	 * dock's own controls do. */
	void openSettings()
	{
		QDialog dialog(this);
		dialog.setWindowTitle(QStringLiteral("IRL Sync Settings"));

		auto *box = new QVBoxLayout(&dialog);
		auto *form = new QFormLayout();

		struct irl_sync_config cfg;
		irl_sync_config_get(&cfg);

		auto *server = new QLineEdit(QString::fromUtf8(cfg.ntp_server),
					     &dialog);
		server->setPlaceholderText(
			QStringLiteral(IRL_SYNC_DEFAULT_NTP_SERVER));
		server->setMinimumWidth(220);
		form->addRow(QStringLiteral("NTP server"), server);
		box->addLayout(form);

		/* The one setting that has to match on equipment this dock
		 * cannot see, which is exactly why it needs saying here. */
		auto *note = new QLabel(
			QStringLiteral(
				"Must be a reference the senders also use.\n"
				"Set the same pool here and in each sending device."),
			&dialog);
		note->setStyleSheet(QString("color: %1;").arg(PANEL_DIM));
		box->addWidget(note);

		auto *buttons = new QDialogButtonBox(
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			&dialog);
		QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
				 &QDialog::accept);
		QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
				 &QDialog::reject);
		box->addWidget(buttons);

		if (dialog.exec() != QDialog::Accepted)
			return;

		irl_sync_set_ntp_server(
			server->text().trimmed().toUtf8().constData());
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
			if (column >= COL_TIMECODE && column <= COL_DRIFT)
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
			/* UTC, not local. The reference is UTC and the senders
			 * stamp their timecodes in it, so rendering these two
			 * rows in local time put the one clock in this dock
			 * that is not on the same zone as the table under it.
			 * (toUTC() rather than a QTimeZone overload: it reads
			 * the same on every Qt 6 the plugin might be built
			 * against.) */
			clockLabel->setText(
				QDateTime::fromMSecsSinceEpoch(ms)
					.toUTC()
					.toString(QStringLiteral(
						"HH:mm:ss.zzz")));
			showingLabel->setText(
				QDateTime::fromMSecsSinceEpoch(ms -
							       cfg.offset_ms)
					.toUTC()
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

		serverCaption->setText(
			ntp.server[0]
				? QString("%1   ·   U T C")
					  .arg(QString::fromUtf8(ntp.server))
				: QStringLiteral("N O   S E R V E R"));

		/* The server itself is the caption above the digits now, so
		 * this line carries only how healthy it is. */
		ntpLabel->setText(health);
		ntpLabel->setStyleSheet(
			QString("color: %1;")
				.arg(fault ? "#f85149" : PANEL_DIM));
	}

	void updateApplyButton()
	{
		applyButton->setEnabled(offsetSpin->value() * 1000 !=
					irl_sync_offset_ms());
	}

	void refreshControls(const struct irl_sync_config &cfg)
	{
		updateMasterButton(cfg.enabled);

		/* Adopt a change that came from somewhere else — a scene
		 * collection load, or the websocket — but never overwrite one
		 * the user has typed and not yet applied. */
		if (cfg.offset_ms != knownOffsetMs) {
			knownOffsetMs = cfg.offset_ms;
			offsetSpin->blockSignals(true);
			offsetSpin->setValue(cfg.offset_ms / 1000);
			offsetSpin->blockSignals(false);
		}

		updateApplyButton();
	}

	void refreshTable()
	{
		struct irl_sync_entry entries[IRL_SYNC_MAX_SOURCES];
		const size_t collected = irl_sync_collect(
			entries, sizeof(entries) / sizeof(entries[0]));

		/* Sources that have not opted in are not part of the group and
		 * have nothing to report, so they are left out entirely rather
		 * than listed as a screenful of dashes. */
		int count = 0;
		for (size_t i = 0; i < collected; i++) {
			if (entries[i].sync_enabled)
				entries[count++] = entries[i];
		}

		if (table->rowCount() != count)
			table->setRowCount(count);

		int alarms = 0;

		for (int row = 0; row < count; row++) {
			const struct irl_sync_entry &e = entries[row];
			const struct irl_sync_snapshot &s = e.snap;

			cell(row, COL_SOURCE)
				->setText(QString::fromUtf8(e.source_name));
			cell(row, COL_TIMECODE)->setText(format_timecode(s));

			const bool measured = s.have_timecode &&
					      s.status != IRL_SYNC_OFF;
			const bool aligning = s.status == IRL_SYNC_LOCKED ||
					      s.status == IRL_SYNC_ACQUIRING;
			cell(row, COL_LATENCY)
				->setText(measured ? format_value(s.latency_ms)
						   : no_value(VALUE_CHARS));
			cell(row, COL_ADDED)
				->setText(aligning ? format_value(s.added_ms)
						   : no_value(VALUE_CHARS));
			cell(row, COL_DRIFT)
				->setText(aligning ? format_value(s.error_ms)
						   : no_value(VALUE_CHARS));

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

		}

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
		} else if (count == 0) {
			/* An empty table otherwise reads as "the dock is
			 * broken" rather than "nothing has opted in". */
			summaryLabel->setText(QStringLiteral(
				"No sources have Sync enabled — tick Sync in a source's properties."));
			summaryLabel->setStyleSheet(
				QString("color: %1;").arg(PANEL_DIM));
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

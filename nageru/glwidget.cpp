#include "glwidget.h"

#include <assert.h>
#include <bmusb/bmusb.h>
#include <movit/effect_chain.h>
#include <movit/resource_pool.h>
#include <stdbool.h>
#include <stdint.h>
#include <QAction>
#include <QActionGroup>
#include <QInputDialog>
#include <QList>
#include <QMenu>
#include <QPoint>
#include <QVariant>
#include <QWidget>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include "audio_mixer.h"
#include "shared/context.h"
#include "context_menus.h"
#include "flags.h"
#include "mainwindow.h"
#include "mixer.h"
#include "shared/ref_counted_gl_sync.h"

class QMouseEvent;

#undef Success
#include <movit/util.h>
#include <string>

using namespace movit;
using namespace std;
using namespace std::placeholders;

namespace {

double srgb_to_linear(double x)
{
	if (x < 0.04045) {
		return x / 12.92;
	} else {
		return pow((x + 0.055) / 1.055, 2.4);
	}
}

}  // namespace

GLWidget::GLWidget(QWidget *parent)
    : QGLWidget(parent, global_share_widget)
{
}

GLWidget::~GLWidget()
{
}

void GLWidget::shutdown()
{
	if (resource_pool != nullptr) {
		makeCurrent();
		resource_pool->clean_context();
	}
	global_mixer->remove_frame_ready_callback(output, this);
}

void GLWidget::grab_white_balance(unsigned channel, unsigned x, unsigned y)
{
	// Set the white balance to neutral for the grab. It's probably going to
	// flicker a bit, but hopefully this display is not live anyway.
	global_mixer->set_wb(output, 1.0, 1.0, 1.0);
	global_mixer->wait_for_next_frame();

	// Mark that the next paintGL() should grab the given pixel.
	grab_x = x;
	grab_y = y;
	grab_output = Mixer::Output(Mixer::OUTPUT_INPUT0 + channel);
	should_grab = true;

	updateGL();
}

void GLWidget::initializeGL()
{
	static once_flag flag;
	call_once(flag, [this]{
		global_mixer = new Mixer(QGLFormat::toSurfaceFormat(format()));
		global_audio_mixer = global_mixer->get_audio_mixer();
		global_mainwindow->mixer_created(global_mixer);
		global_mixer->start();
	});
	global_mixer->add_frame_ready_callback(output, this, [this]{
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	});
	global_mixer->set_name_updated_callback(output, [this](const string &name){
		emit name_updated(output, name);
	});
	if (output == Mixer::OUTPUT_LIVE) {
		global_mixer->set_transition_names_updated_callback(output, [this](const vector<string> &names){
			emit transition_names_updated(names);
		});
	}
	if (output >= Mixer::OUTPUT_INPUT0) {
		global_mixer->set_color_updated_callback(output, [this](const string &color){
			emit color_updated(output, color);
		});
	}
	setContextMenuPolicy(Qt::CustomContextMenu);
	connect(this, &QWidget::customContextMenuRequested, bind(&GLWidget::show_context_menu, this, _1));

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
}

void GLWidget::resizeGL(int width, int height)
{
	current_width = width;
	current_height = height;
	glViewport(0, 0, width, height);
}

void GLWidget::paintGL()
{
	Mixer::DisplayFrame frame;
	if (!global_mixer->get_display_frame(output, &frame)) {
		glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
		check_error();
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		check_error();
		return;
	}

	check_error();
	glWaitSync(frame.ready_fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
	check_error();
	frame.setup_chain();
	check_error();
	glDisable(GL_FRAMEBUFFER_SRGB);
	check_error();
	frame.chain->render_to_fbo(0, current_width, current_height);
	check_error();

	if (resource_pool == nullptr) {
		resource_pool = frame.chain->get_resource_pool();
	} else {
		assert(resource_pool == frame.chain->get_resource_pool());
	}

	if (should_grab) {
		GLfloat reference_color[4];
		glReadPixels(grab_x, current_height - grab_y - 1, 1, 1, GL_BGRA, GL_FLOAT, reference_color);

		double r = srgb_to_linear(reference_color[2]);
		double g = srgb_to_linear(reference_color[1]);
		double b = srgb_to_linear(reference_color[0]);
		global_mixer->set_wb(grab_output, r, g, b);
		should_grab = false;
	}
}

void GLWidget::mousePressEvent(QMouseEvent *event)
{
	emit clicked();
}

void GLWidget::show_context_menu(const QPoint &pos)
{
	if (output == Mixer::OUTPUT_LIVE) {
		show_live_context_menu(pos);
	}
	if (output >= Mixer::OUTPUT_INPUT0) {
		int signal_num = global_mixer->map_channel_to_signal(output);
		if (signal_num != -1) {
			show_preview_context_menu(signal_num, pos);
		}
	}
}

void GLWidget::show_live_context_menu(const QPoint &pos)
{
	QPoint global_pos = mapToGlobal(pos);

	QMenu menu;

	// Add a submenu for selecting output card, with an action for each card.
	QMenu card_submenu;
	fill_hdmi_sdi_output_device_menu(&card_submenu);
	card_submenu.setTitle("HDMI/SDI output device");
	menu.addMenu(&card_submenu);

	// Add a submenu for choosing the output resolution. Since this is
	// card-dependent, it is disabled if we haven't chosen a card
	// (but it's still there so that the user will know it exists).
	QMenu resolution_submenu;
	fill_hdmi_sdi_output_resolution_menu(&resolution_submenu);
	resolution_submenu.setTitle("HDMI/SDI output resolution");
	menu.addMenu(&resolution_submenu);

	// Show the menu; if there's an action selected, it will deal with it itself.
	menu.exec(global_pos);
}

void GLWidget::show_preview_context_menu(unsigned signal_num, const QPoint &pos)
{
	QPoint global_pos = mapToGlobal(pos);

	QMenu menu;

	// Add a submenu for selecting input card, with an action for each card.
	QMenu card_submenu;
	QActionGroup card_group(&card_submenu);

	QMenu interpretation_submenu;
	QActionGroup interpretation_group(&interpretation_submenu);

	QMenu video_input_submenu;
	QActionGroup video_input_group(&video_input_submenu);

	QMenu audio_input_submenu;
	QActionGroup audio_input_group(&audio_input_submenu);

	QMenu mode_submenu;
	QActionGroup mode_group(&mode_submenu);

	unsigned current_card = global_mixer->map_signal_to_card(signal_num);
	bool is_ffmpeg = global_mixer->card_is_ffmpeg(current_card);

	if (!is_ffmpeg) {  // FFmpeg inputs are not connected to any card; they're locked to a given input and have a given Y'CbCr interpretatio and have a given Y'CbCr interpretation.
		for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
			if (!global_mixer->card_is_active(card_index)) continue;
			if (global_mixer->card_is_cef(card_index) || global_mixer->card_is_ffmpeg(card_index)) continue;
			QString description(QString::fromStdString(global_mixer->get_card_description(card_index)));
			QAction *action = new QAction(description, &card_group);
			action->setCheckable(true);
			if (current_card == card_index) {
				action->setChecked(true);
			}
			action->setData(QList<QVariant>{"card", card_index});
			card_submenu.addAction(action);
		}

		card_submenu.setTitle("Input source");
		menu.addMenu(&card_submenu);

		// Note that this setting depends on which card is active.

		YCbCrInterpretation current_interpretation = global_mixer->get_input_ycbcr_interpretation(current_card);
		{
			QAction *action = new QAction("Auto", &interpretation_group);
			action->setCheckable(true);
			if (current_interpretation.ycbcr_coefficients_auto) {
				action->setChecked(true);
			}
			action->setData(QList<QVariant>{"interpretation", true, YCBCR_REC_709, false});
			interpretation_submenu.addAction(action);
		}
		for (YCbCrLumaCoefficients ycbcr_coefficients : { YCBCR_REC_709, YCBCR_REC_601 }) {
			for (bool full_range : { false, true }) {
				std::string description;
				if (ycbcr_coefficients == YCBCR_REC_709) {
					description = "Rec. 709 (HD)";
				} else {
					description = "Rec. 601 (SD)";
				}
				if (full_range) {
					description += ", full range (nonstandard)";
				}
				QAction *action = new QAction(QString::fromStdString(description), &interpretation_group);
				action->setCheckable(true);
				if (!current_interpretation.ycbcr_coefficients_auto &&
				    ycbcr_coefficients == current_interpretation.ycbcr_coefficients &&
				    full_range == current_interpretation.full_range) {
					action->setChecked(true);
				}
				action->setData(QList<QVariant>{"interpretation", false, ycbcr_coefficients, full_range});
				interpretation_submenu.addAction(action);
			}
		}

		interpretation_submenu.setTitle("Input interpretation");
		menu.addMenu(&interpretation_submenu);
	}

	// --- The choices in the next few options depend a lot on which card is active ---

	bool has_auto_mode = false;
	QAction *change_url_action = nullptr;
	if (is_ffmpeg) {
		// Add a menu to change the source URL if we're an FFmpeg card.
		// (The theme can still override.)
		change_url_action = new QAction("Change source filename/URL…", &menu);
		menu.addAction(change_url_action);
	} else {
		// Add a submenu for selecting video input, with an action for each input.
		std::map<uint32_t, string> video_inputs = global_mixer->get_available_video_inputs(current_card);
		uint32_t current_video_input = global_mixer->get_current_video_input(current_card);
		for (const auto &mode : video_inputs) {
			QString description(QString::fromStdString(mode.second));
			QAction *action = new QAction(description, &video_input_group);
			action->setCheckable(true);
			if (mode.first == current_video_input) {
				action->setChecked(true);
			}
			action->setData(QList<QVariant>{"video_input", mode.first});
			video_input_submenu.addAction(action);
		}

		video_input_submenu.setTitle("Video input");
		menu.addMenu(&video_input_submenu);

		// The same for audio input.
		std::map<uint32_t, string> audio_inputs = global_mixer->get_available_audio_inputs(current_card);
		uint32_t current_audio_input = global_mixer->get_current_audio_input(current_card);
		for (const auto &mode : audio_inputs) {
			QString description(QString::fromStdString(mode.second));
			QAction *action = new QAction(description, &audio_input_group);
			action->setCheckable(true);
			if (mode.first == current_audio_input) {
				action->setChecked(true);
			}
			action->setData(QList<QVariant>{"audio_input", mode.first});
			audio_input_submenu.addAction(action);
		}

		audio_input_submenu.setTitle("Audio input");
		menu.addMenu(&audio_input_submenu);

		// The same for resolution.
		std::map<uint32_t, bmusb::VideoMode> video_modes = global_mixer->get_available_video_modes(current_card);
		uint32_t current_video_mode = global_mixer->get_current_video_mode(current_card);
		for (const auto &mode : video_modes) {
			QString description(QString::fromStdString(mode.second.name));
			QAction *action = new QAction(description, &mode_group);
			action->setCheckable(true);
			if (mode.first == current_video_mode) {
				action->setChecked(true);
			}
			action->setData(QList<QVariant>{"video_mode", mode.first});
			mode_submenu.addAction(action);

			// TODO: Relying on the 0 value here (from bmusb.h) is ugly, it should be a named constant.
			if (mode.first == 0) {
				has_auto_mode = true;
			}
		}

		// Add a “scan” menu if there's no “auto” mode.
		if (!has_auto_mode) {
			QAction *action = new QAction("Scan", &mode_group);
			action->setData(QList<QVariant>{"video_mode", 0});
			mode_submenu.addSeparator();
			mode_submenu.addAction(action);
		}

		mode_submenu.setTitle("Input mode");
		menu.addMenu(&mode_submenu);
	}

	// --- End of card-dependent choices ---

	// Add an audio source selector.
	QAction *audio_source_action = nullptr;
	if (global_audio_mixer->get_mapping_mode() == AudioMixer::MappingMode::SIMPLE) {
		audio_source_action = new QAction("Use as audio source", &menu);
		audio_source_action->setCheckable(true);
		if (global_audio_mixer->get_simple_input() == current_card) {
			audio_source_action->setChecked(true);
			audio_source_action->setEnabled(false);
		}
		menu.addAction(audio_source_action);
	}

	// And a master clock selector.
	QAction *master_clock_action = new QAction("Use as master clock", &menu);
	master_clock_action->setCheckable(true);
	if (global_mixer->get_output_card_index() != -1 && global_mixer->get_output_card_is_master()) {
		master_clock_action->setChecked(false);
		master_clock_action->setEnabled(false);
	} else if (global_mixer->get_master_clock() == signal_num) {
		master_clock_action->setChecked(true);
		master_clock_action->setEnabled(false);
	}
	menu.addAction(master_clock_action);

	// Show the menu and look at the result.
	QAction *selected_item = menu.exec(global_pos);
	if (audio_source_action != nullptr && selected_item == audio_source_action) {
		global_audio_mixer->set_simple_input(current_card);
	} else if (change_url_action != nullptr && selected_item == change_url_action) {
		// NOTE: We can't use “this” as parent, since the dialog would inherit our style sheet.
		bool ok;
		const string url = global_mixer->get_ffmpeg_filename(current_card);
		QString new_url = QInputDialog::getText(window(), tr("Change URL"),
			tr("Enter new filename/URL for the given video input:"), QLineEdit::Normal,
				QString::fromStdString(url), &ok);
		// FIXME prefill the input
		if (ok) {
			global_mixer->set_ffmpeg_filename(current_card, new_url.toStdString());
		}
	} else if (selected_item == master_clock_action) {
		global_mixer->set_master_clock(signal_num);
	} else if (selected_item != nullptr) {
		QList<QVariant> selected = selected_item->data().toList();
		if (selected[0].toString() == "video_mode") {
			uint32_t mode = selected[1].toUInt(nullptr);
			if (mode == 0 && !has_auto_mode) {
				global_mixer->start_mode_scanning(current_card);
			} else {
				global_mixer->set_video_mode(current_card, mode);
			}
		} else if (selected[0].toString() == "video_input") {
			uint32_t input = selected[1].toUInt(nullptr);
			global_mixer->set_video_input(current_card, input);
		} else if (selected[0].toString() == "audio_input") {
			uint32_t input = selected[1].toUInt(nullptr);
			global_mixer->set_audio_input(current_card, input);
		} else if (selected[0].toString() == "card") {
			unsigned card_index = selected[1].toUInt(nullptr);
			global_mixer->set_signal_mapping(signal_num, card_index);
		} else if (selected[0].toString() == "interpretation") {
			YCbCrInterpretation interpretation;
			interpretation.ycbcr_coefficients_auto = selected[1].toBool();
			interpretation.ycbcr_coefficients = YCbCrLumaCoefficients(selected[2].toUInt(nullptr));
			interpretation.full_range = selected[3].toBool();
			global_mixer->set_input_ycbcr_interpretation(current_card, interpretation);
		} else {
			assert(false);
		}
	}
}

/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2022-2023, Raspberry Pi Ltd
 *
 * af.cpp - Autofocus control algorithm
 */

#include "af.h"

#include <iomanip>
#include <math.h>
#include <stdlib.h>

#include <libcamera/base/log.h>

#include <libcamera/control_ids.h>

using namespace RPiController;
using namespace libcamera;

LOG_DEFINE_CATEGORY(RPiAf)

#define NAME "rpi.af"

/*
 * Default values for parameters. All may be overridden in the tuning file.
 * Many of these values are sensor- or module-dependent; the defaults here
 * assume IMX708 in a Raspberry Pi V3 camera with the standard lens.
 *
 * Here all focus values are in dioptres (1/m). They are converted to hardware
 * units when written to status.lensSetting or returned from setLensPosition().
 *
 * Gain and delay values are relative to the update rate, since much (not all)
 * of the delay is in the sensor and (for CDAF) ISP, not the lens mechanism;
 * but note that algorithms are updated at no more than 30 Hz.
 */

Af::RangeDependentParams::RangeDependentParams()
	: focusMin(0.0),
	  focusMax(12.0),
	  focusDefault(1.0)
{
}

Af::SpeedDependentParams::SpeedDependentParams()
	: stepCoarse(1.0),
	  stepFine(0.25),
	  contrastRatio(0.75),
	  pdafGain(-0.02),
	  pdafSquelch(0.125),
	  maxSlew(2.0),
	  pdafFrames(20),
	  dropoutFrames(6),
	  stepFrames(4)
{
}

Af::CfgParams::CfgParams()
	: confEpsilon(8),
	  confThresh(16),
	  confClip(512),
	  skipFrames(5),
	  map()
{
}

template<typename T>
static void readNumber(T &dest, const libcamera::YamlObject &params, char const *name)
{
	auto value = params[name].get<T>();
	if (value)
		dest = *value;
	else
		LOG(RPiAf, Warning) << "Missing parameter \"" << name << "\"";
}

void Af::RangeDependentParams::read(const libcamera::YamlObject &params)
{

	readNumber<double>(focusMin, params, "min");
	readNumber<double>(focusMax, params, "max");
	readNumber<double>(focusDefault, params, "default");
}

void Af::SpeedDependentParams::read(const libcamera::YamlObject &params)
{
	readNumber<double>(stepCoarse, params, "step_coarse");
	readNumber<double>(stepFine, params, "step_fine");
	readNumber<double>(contrastRatio, params, "contrast_ratio");
	readNumber<double>(pdafGain, params, "pdaf_gain");
	readNumber<double>(pdafSquelch, params, "pdaf_squelch");
	readNumber<double>(maxSlew, params, "max_slew");
	readNumber<uint32_t>(pdafFrames, params, "pdaf_frames");
	readNumber<uint32_t>(dropoutFrames, params, "dropout_frames");
	readNumber<uint32_t>(stepFrames, params, "step_frames");
}

int Af::CfgParams::read(const libcamera::YamlObject &params)
{
	if (params.contains("ranges")) {
		auto &rr = params["ranges"];

		if (rr.contains("normal"))
			ranges[AfRangeNormal].read(rr["normal"]);
		else
			LOG(RPiAf, Warning) << "Missing range \"normal\"";

		ranges[AfRangeMacro] = ranges[AfRangeNormal];
		if (rr.contains("macro"))
			ranges[AfRangeMacro].read(rr["macro"]);

		ranges[AfRangeFull].focusMin = std::min(ranges[AfRangeNormal].focusMin,
							ranges[AfRangeMacro].focusMin);
		ranges[AfRangeFull].focusMax = std::max(ranges[AfRangeNormal].focusMax,
							ranges[AfRangeMacro].focusMax);
		ranges[AfRangeFull].focusDefault = ranges[AfRangeNormal].focusDefault;
		if (rr.contains("full"))
			ranges[AfRangeFull].read(rr["full"]);
	} else
		LOG(RPiAf, Warning) << "No ranges defined";

	if (params.contains("speeds")) {
		auto &ss = params["speeds"];

		if (ss.contains("normal"))
			speeds[AfSpeedNormal].read(ss["normal"]);
		else
			LOG(RPiAf, Warning) << "Missing speed \"normal\"";

		speeds[AfSpeedFast] = speeds[AfSpeedNormal];
		if (ss.contains("fast"))
			speeds[AfSpeedFast].read(ss["fast"]);
	} else
		LOG(RPiAf, Warning) << "No speeds defined";

	readNumber<uint32_t>(confEpsilon, params, "conf_epsilon");
	readNumber<uint32_t>(confThresh, params, "conf_thresh");
	readNumber<uint32_t>(confClip, params, "conf_clip");
	readNumber<uint32_t>(skipFrames, params, "skip_frames");

	if (params.contains("map"))
		map.read(params["map"]);
	else
		LOG(RPiAf, Warning) << "No map defined";

	return 0;
}

void Af::CfgParams::initialise()
{
	if (map.empty()) {
		/* Default mapping from dioptres to hardware setting */
		static constexpr double DefaultMapX0 = 0.0;
		static constexpr double DefaultMapY0 = 445.0;
		static constexpr double DefaultMapX1 = 15.0;
		static constexpr double DefaultMapY1 = 925.0;

		map.append(DefaultMapX0, DefaultMapY0);
		map.append(DefaultMapX1, DefaultMapY1);
	}
}

/* Af Algorithm class */

static constexpr unsigned MaxWindows = 10;

Af::Af(Controller *controller)
	: AfAlgorithm(controller),
	  cfg_(),
	  range_(AfRangeNormal),
	  speed_(AfSpeedNormal),
	  mode_(AfAlgorithm::AfModeManual),
	  pauseFlag_(false),
	  statsRegion_(0, 0, 0, 0),
	  windows_(),
	  useWindows_(false),
	  phaseWeights_{},
	  contrastWeights_{},
	  sumWeights_(0),
	  scanState_(ScanState::Idle),
	  initted_(false),
	  ftarget_(-1.0),
	  fsmooth_(-1.0),
	  prevContrast_(0.0),
	  skipCount_(0),
	  stepCount_(0),
	  dropCount_(0),
	  scanMaxContrast_(0.0),
	  scanMinContrast_(1.0e9),
	  scanData_(),
	  reportState_(AfState::Idle)
{
	scanData_.reserve(24);
}

Af::~Af()
{
}

char const *Af::name() const
{
	return NAME;
}

int Af::read(const libcamera::YamlObject &params)
{
	return cfg_.read(params);
}

void Af::initialise()
{
	cfg_.initialise();
}

void Af::switchMode(CameraMode const &cameraMode, [[maybe_unused]] Metadata *metadata)
{
	(void)metadata;

	/* Assume that PDAF and Focus stats grids cover the visible area */
	statsRegion_.x = (int)cameraMode.cropX;
	statsRegion_.y = (int)cameraMode.cropY;
	statsRegion_.width = (unsigned)(cameraMode.width * cameraMode.scaleX);
	statsRegion_.height = (unsigned)(cameraMode.height * cameraMode.scaleY);
	LOG(RPiAf, Debug) << "switchMode: statsRegion: "
			  << statsRegion_.x << ','
			  << statsRegion_.y << ','
			  << statsRegion_.width << ','
			  << statsRegion_.height;
	computeWeights();

	if (scanState_ >= ScanState::Coarse && scanState_ < ScanState::Settle) {
		/*
		 * If a scan was in progress, re-start it, as CDAF statistics
		 * may have changed. Though if the application is just about
		 * to take a still picture, this will not help...
		 */
		startProgrammedScan();
	}
	skipCount_ = cfg_.skipFrames;
}

void Af::computeWeights()
{
	constexpr int MaxCellWeight = 240 / (int)MaxWindows;

	sumWeights_ = 0;
	for (int i = 0; i < PDAF_DATA_ROWS; ++i)
		std::fill(phaseWeights_[i], phaseWeights_[i] + PDAF_DATA_COLS, 0);

	if (useWindows_ &&
	    statsRegion_.width >= PDAF_DATA_COLS && statsRegion_.height >= PDAF_DATA_ROWS) {
		/*
		 * Here we just merge all of the given windows, weighted by area.
		 * \todo Perhaps a better approach might be to find the phase in each
		 * window and choose either the closest or the highest-confidence one?
		 *
		 * Using mostly "int" arithmetic, because Rectangle has signed x, y
		 */
		int cellH = (int)(statsRegion_.height / PDAF_DATA_ROWS);
		int cellW = (int)(statsRegion_.width / PDAF_DATA_COLS);
		int cellA = cellH * cellW;

		for (auto &w : windows_) {
			for (int i = 0; i < PDAF_DATA_ROWS; ++i) {
				int y0 = std::max(statsRegion_.y + cellH * i, w.y);
				int y1 = std::min(statsRegion_.y + cellH * (i + 1), w.y + (int)(w.height));
				if (y0 >= y1)
					continue;
				y1 -= y0;
				for (int j = 0; j < PDAF_DATA_COLS; ++j) {
					int x0 = std::max(statsRegion_.x + cellW * j, w.x);
					int x1 = std::min(statsRegion_.x + cellW * (j + 1), w.x + (int)(w.width));
					if (x0 >= x1)
						continue;
					int a = y1 * (x1 - x0);
					a = (MaxCellWeight * a + cellA - 1) / cellA;
					phaseWeights_[i][j] += a;
					sumWeights_ += a;
				}
			}
		}
	}

	if (sumWeights_ == 0) {
		/*
		 * Default AF window is the middle 1/2 width of the middle 1/3 height
		 * since this maps nicely to both PDAF (16x12) and Focus (4x3) grids.
		 */
		for (int i = PDAF_DATA_ROWS / 3; i < 2 * PDAF_DATA_ROWS / 3; ++i) {
			for (int j = PDAF_DATA_COLS / 4; j < 3 * PDAF_DATA_COLS / 4; ++j) {
				phaseWeights_[i][j] = MaxCellWeight;
				sumWeights_ += MaxCellWeight;
			}
		}
	}

	/* Scale from PDAF to Focus Statistics grid (which has fixed size 4x3) */
	constexpr int FocusStatsRows = 3;
	constexpr int FocusStatsCols = 4;
	static_assert(FOCUS_REGIONS == FocusStatsRows * FocusStatsCols);
	static_assert(PDAF_DATA_ROWS % FocusStatsRows == 0);
	static_assert(PDAF_DATA_COLS % FocusStatsCols == 0);
	constexpr int YFactor = PDAF_DATA_ROWS / FocusStatsRows;
	constexpr int XFactor = PDAF_DATA_COLS / FocusStatsCols;

	LOG(RPiAf, Debug) << "Recomputed weights:";
	for (int i = 0; i < FocusStatsRows; ++i) {
		for (int j = 0; j < FocusStatsCols; ++j) {
			unsigned w = 0;
			for (int y = 0; y < YFactor; ++y)
				for (int x = 0; x < XFactor; ++x)
					w += phaseWeights_[YFactor * i + y][XFactor * j + x];
			contrastWeights_[FocusStatsCols * i + j] = w;
		}
		LOG(RPiAf, Debug) << "   "
				  << contrastWeights_[FocusStatsCols * i + 0] << " "
				  << contrastWeights_[FocusStatsCols * i + 1] << " "
				  << contrastWeights_[FocusStatsCols * i + 2] << " "
				  << contrastWeights_[FocusStatsCols * i + 3];
	}
}

bool Af::getPhase(PdafData const &data, double &phase, double &conf) const
{
	uint32_t sumWc = 0;
	int64_t sumWcp = 0;

	for (unsigned i = 0; i < PDAF_DATA_ROWS; ++i) {
		for (unsigned j = 0; j < PDAF_DATA_COLS; ++j) {
			if (phaseWeights_[i][j]) {
				uint32_t c = data.conf[i][j];
				if (c >= cfg_.confThresh) {
					if (c > cfg_.confClip)
						c = cfg_.confClip;
					c -= (cfg_.confThresh >> 2);
					sumWc += phaseWeights_[i][j] * c;
					c -= (cfg_.confThresh >> 2);
					sumWcp += phaseWeights_[i][j] * data.phase[i][j] * (int64_t)c;
				}
			}
		}
	}

	if (0 < sumWeights_ && sumWeights_ <= sumWc) {
		phase = (double)sumWcp / (double)sumWc;
		conf = (double)sumWc / (double)sumWeights_;
		return true;
	} else {
		phase = 0.0;
		conf = 0.0;
		return false;
	}
}

double Af::getContrast(struct bcm2835_isp_stats_focus const focus_stats[FOCUS_REGIONS]) const
{
	uint32_t sumWc = 0;

	for (unsigned i = 0; i < FOCUS_REGIONS; ++i) {
		unsigned w = contrastWeights_[i];
		sumWc += w * (focus_stats[i].contrast_val[1][1] >> 10);
	}

	return (sumWeights_ == 0) ? 0.0 : (double)sumWc / (double)sumWeights_;
}

void Af::doPDAF(double phase, double conf)
{
	/* Apply loop gain */
	phase *= cfg_.speeds[speed_].pdafGain;

	if (mode_ == AfModeContinuous) {
		/*
		 * PDAF in Continuous mode. Scale down lens movement when
		 * delta is small or confidence is low, to suppress wobble.
		 */
		phase *= conf / (conf + cfg_.confEpsilon);
		if (std::abs(phase) < cfg_.speeds[speed_].pdafSquelch) {
			double a = phase / cfg_.speeds[speed_].pdafSquelch;
			phase *= a * a;
		}
	} else {
		/*
		 * PDAF in triggered-auto mode. Allow early termination when
		 * phase delta is small; scale down lens movements towards
		 * the end of the sequence, to ensure a stable image.
		 */
		if (stepCount_ >= cfg_.speeds[speed_].stepFrames) {
			if (std::abs(phase) < cfg_.speeds[speed_].pdafSquelch)
				stepCount_ = cfg_.speeds[speed_].stepFrames;
		} else
			phase *= stepCount_ / cfg_.speeds[speed_].stepFrames;
	}

	/* Apply slew rate limit. Report failure if out of bounds. */
	if (phase < -cfg_.speeds[speed_].maxSlew) {
		phase = -cfg_.speeds[speed_].maxSlew;
		reportState_ = (ftarget_ <= cfg_.ranges[range_].focusMin) ? AfState::Failed
									  : AfState::Scanning;
	} else if (phase > cfg_.speeds[speed_].maxSlew) {
		phase = cfg_.speeds[speed_].maxSlew;
		reportState_ = (ftarget_ >= cfg_.ranges[range_].focusMax) ? AfState::Failed
									  : AfState::Scanning;
	} else
		reportState_ = AfState::Focused;

	ftarget_ = fsmooth_ + phase;
}

bool Af::earlyTerminationByPhase(double phase)
{
	if (scanData_.size() > 0 &&
	    scanData_[scanData_.size() - 1].conf >= cfg_.confEpsilon) {
		double oldFocus = scanData_[scanData_.size() - 1].focus;
		double oldPhase = scanData_[scanData_.size() - 1].phase;

		/*
		 * Check that the gradient is finite and has the expected sign;
		 * Interpolate/extrapolate the lens position for zero phase.
		 * Check that the extrapolation is well-conditioned.
		 */
		if ((ftarget_ - oldFocus) * (phase - oldPhase) > 0.0) {
			double param = phase / (phase - oldPhase);
			if (-3.0 <= param && param <= 3.5) {
				ftarget_ += param * (oldFocus - ftarget_);
				LOG(RPiAf, Debug) << "ETBP: param=" << param;
				return true;
			}
		}
	}

	return false;
}

double Af::findPeak(unsigned i) const
{
	double f = scanData_[i].focus;

	if (i > 0 && i + 1 < scanData_.size()) {
		double dropLo = scanData_[i].contrast - scanData_[i - 1].contrast;
		double dropHi = scanData_[i].contrast - scanData_[i + 1].contrast;
		if (0.0 <= dropLo && dropLo < dropHi) {
			double param = 0.3125 * (1.0 - dropLo / dropHi) * (1.6 - dropLo / dropHi);
			f += param * (scanData_[i - 1].focus - f);
		} else if (0.0 <= dropHi && dropHi < dropLo) {
			double param = 0.3125 * (1.0 - dropHi / dropLo) * (1.6 - dropHi / dropLo);
			f += param * (scanData_[i + 1].focus - f);
		}
	}

	LOG(RPiAf, Debug) << "FindPeak: " << f;
	return f;
}

void Af::doScan(double contrast, double phase, double conf)
{
	/* Record lens position, contrast and phase values for the current scan */
	if (scanData_.empty() || contrast > scanMaxContrast_) {
		scanMaxContrast_ = contrast;
		scanMaxIndex_ = scanData_.size();
	}
	if (contrast < scanMinContrast_)
		scanMinContrast_ = contrast;
	scanData_.emplace_back(ScanRecord{ ftarget_, contrast, phase, conf });

	if (scanState_ == ScanState::Coarse) {
		if (ftarget_ >= cfg_.ranges[range_].focusMax ||
		    contrast < cfg_.speeds[speed_].contrastRatio * scanMaxContrast_) {
			/*
			 * Finished course scan, or termination based on contrast.
			 * Jump to just after max contrast and start fine scan.
			 */
			ftarget_ = std::min(ftarget_, findPeak(scanMaxIndex_) +
					2.0 * cfg_.speeds[speed_].stepFine);
			scanState_ = ScanState::Fine;
			scanData_.clear();
		} else
			ftarget_ += cfg_.speeds[speed_].stepCoarse;
	} else { /* ScanState::Fine */
		if (ftarget_ <= cfg_.ranges[range_].focusMin || scanData_.size() >= 5 ||
		    contrast < cfg_.speeds[speed_].contrastRatio * scanMaxContrast_) {
			/*
			 * Finished fine scan, or termination based on contrast.
			 * Use quadratic peak-finding to find best contrast position.
			 */
			ftarget_ = findPeak(scanMaxIndex_);
			scanState_ = ScanState::Settle;
		} else
			ftarget_ -= cfg_.speeds[speed_].stepFine;
	}

	stepCount_ = (ftarget_ == fsmooth_) ? 0 : cfg_.speeds[speed_].stepFrames;
}

void Af::doAF(double contrast, double phase, double conf)
{
	/* Skip frames at startup and after sensor mode change */
	if (skipCount_ > 0) {
		LOG(RPiAf, Debug) << "SKIP";
		skipCount_--;
		return;
	}

	if (scanState_ == ScanState::Pdaf) {
		/*
		 * Use PDAF closed-loop control whenever available, in both CAF
		 * mode and (for a limited number of iterations) when triggered.
		 * If PDAF fails (due to poor contrast, noise or large defocus),
		 * fall back to a CDAF-based scan. To avoid "nuisance" scans,
		 * scan only after a number of frames with low PDAF confidence.
		 */
		if (conf > (dropCount_ ? 1.0 : 0.25) * cfg_.confEpsilon) {
			doPDAF(phase, conf);
			if (stepCount_ > 0)
				stepCount_--;
			else if (mode_ != AfModeContinuous)
				scanState_ = ScanState::Idle;
			dropCount_ = 0;
		} else if (++dropCount_ == cfg_.speeds[speed_].dropoutFrames)
			startProgrammedScan();
	} else if (scanState_ >= ScanState::Coarse && fsmooth_ == ftarget_) {
		/*
		 * Scanning sequence. This means PDAF has become unavailable.
		 * Allow a delay between steps for CDAF FoM statistics to be
		 * updated, and a "settling time" at the end of the sequence.
		 * [A coarse or fine scan can be abandoned if two PDAF samples
		 * allow direct interpolation of the zero-phase lens position.]
		 */
		if (stepCount_ > 0)
			stepCount_--;
		else if (scanState_ == ScanState::Settle) {
			if (prevContrast_ >= cfg_.speeds[speed_].contrastRatio * scanMaxContrast_ &&
			    scanMinContrast_ <= cfg_.speeds[speed_].contrastRatio * scanMaxContrast_)
				reportState_ = AfState::Focused;
			else
				reportState_ = AfState::Failed;
			if (mode_ == AfModeContinuous && !pauseFlag_ &&
			    cfg_.speeds[speed_].dropoutFrames > 0)
				scanState_ = ScanState::Pdaf;
			else
				scanState_ = ScanState::Idle;
			scanData_.clear();
		} else if (conf >= cfg_.confEpsilon && earlyTerminationByPhase(phase)) {
			scanState_ = ScanState::Settle;
			stepCount_ = (mode_ == AfModeContinuous) ? 0
								 : cfg_.speeds[speed_].stepFrames;
		} else
			doScan(contrast, phase, conf);
	}
}

void Af::updateLensPosition()
{
	if (scanState_ >= ScanState::Pdaf) {
		ftarget_ = std::clamp(ftarget_,
				      cfg_.ranges[range_].focusMin,
				      cfg_.ranges[range_].focusMax);
	}

	if (initted_) {
		/* from a known lens position: apply slew rate limit */
		fsmooth_ = std::clamp(ftarget_,
				      fsmooth_ - cfg_.speeds[speed_].maxSlew,
				      fsmooth_ + cfg_.speeds[speed_].maxSlew);
	} else {
		/* from an unknown position: go straight to target, but add delay */
		fsmooth_ = ftarget_;
		initted_ = true;
		skipCount_ = cfg_.skipFrames;
	}
}

void Af::startAF()
{
	/* Use PDAF if the tuning file allows it; else CDAF. */
	if (cfg_.speeds[speed_].dropoutFrames > 0 &&
	    (mode_ == AfModeContinuous || cfg_.speeds[speed_].pdafFrames > 0)) {
		if (!initted_) {
			ftarget_ = cfg_.ranges[range_].focusDefault;
			updateLensPosition();
		}
		stepCount_ = (mode_ == AfModeContinuous) ? 0 : cfg_.speeds[speed_].pdafFrames;
		scanState_ = ScanState::Pdaf;
		scanData_.clear();
		dropCount_ = 0;
		reportState_ = AfState::Scanning;
	} else
		startProgrammedScan();
}

void Af::startProgrammedScan()
{
	ftarget_ = cfg_.ranges[range_].focusMin;
	updateLensPosition();
	scanState_ = ScanState::Coarse;
	scanMaxContrast_ = 0.0;
	scanMinContrast_ = 1.0e9;
	scanMaxIndex_ = 0;
	scanData_.clear();
	stepCount_ = cfg_.speeds[speed_].stepFrames;
	reportState_ = AfState::Scanning;
}

void Af::goIdle()
{
	scanState_ = ScanState::Idle;
	reportState_ = AfState::Idle;
	scanData_.clear();
}

/*
 * PDAF phase data are available in prepare(), but CDAF statistics are not
 * available until process(). We are gambling on the availability of PDAF.
 * To expedite feedback control using PDAF, issue the V4L2 lens control from
 * prepare(). Conversely, during scans, we must allow an extra frame delay
 * between steps, to retrieve CDAF statistics from the previous process()
 * so we can terminate the scan early without having to change our minds.
 */

void Af::prepare(Metadata *imageMetadata)
{
	/* Initialize for triggered scan or start of CAF mode */
	if (scanState_ == ScanState::Trigger)
		startAF();

	if (initted_) {
		/* Get PDAF from the embedded metadata, and run AF algorithm core */
		PdafData data;
		double phase = 0.0, conf = 0.0;
		double oldFt = ftarget_;
		double oldFs = fsmooth_;
		ScanState oldSs = scanState_;
		uint32_t oldSt = stepCount_;
		if (imageMetadata->get("pdaf.data", data) == 0)
			getPhase(data, phase, conf);
		doAF(prevContrast_, phase, conf);
		updateLensPosition();
		LOG(RPiAf, Debug) << std::fixed << std::setprecision(2)
				  << static_cast<unsigned int>(reportState_)
				  << " sst" << static_cast<unsigned int>(oldSs)
				  << "->" << static_cast<unsigned int>(scanState_)
				  << " stp" << oldSt << "->" << stepCount_
				  << " ft" << oldFt << "->" << ftarget_
				  << " fs" << oldFs << "->" << fsmooth_
				  << " cont=" << (int)prevContrast_
				  << " phase=" << (int)phase << " conf=" << (int)conf;
	}

	/* Report status and produce new lens setting */
	AfStatus status;
	if (pauseFlag_)
		status.pauseState = (scanState_ == ScanState::Idle) ? AfPauseState::Paused
								    : AfPauseState::Pausing;
	else
		status.pauseState = AfPauseState::Running;

	if (mode_ == AfModeAuto && scanState_ != ScanState::Idle)
		status.state = AfState::Scanning;
	else
		status.state = reportState_;
	status.lensSetting = initted_ ? std::optional<int>(cfg_.map.eval(fsmooth_))
				      : std::nullopt;
	imageMetadata->set("af.status", status);
}

void Af::process(StatisticsPtr &stats, [[maybe_unused]] Metadata *imageMetadata)
{
	(void)imageMetadata;
	prevContrast_ = getContrast(stats->focus_stats);
}

/* Controls */

void Af::setRange(AfRange r)
{
	LOG(RPiAf, Debug) << "setRange: " << (unsigned)r;
	if (r < AfAlgorithm::AfRangeMax)
		range_ = r;
}

void Af::setSpeed(AfSpeed s)
{
	LOG(RPiAf, Debug) << "setSpeed: " << (unsigned)s;
	if (s < AfAlgorithm::AfSpeedMax) {
		if (scanState_ == ScanState::Pdaf &&
		    cfg_.speeds[s].pdafFrames > cfg_.speeds[speed_].pdafFrames)
			stepCount_ += cfg_.speeds[s].pdafFrames - cfg_.speeds[speed_].pdafFrames;
		speed_ = s;
	}
}

void Af::setMetering(bool mode)
{
	if (useWindows_ != mode) {
		useWindows_ = mode;
		computeWeights();
	}
}

void Af::setWindows(libcamera::Span<libcamera::Rectangle const> const &wins)
{
	windows_.clear();
	for (auto &w : wins) {
		LOG(RPiAf, Debug) << "Window: "
				  << w.x << ", "
				  << w.y << ", "
				  << w.width << ", "
				  << w.height;
		windows_.push_back(w);
		if (windows_.size() >= MaxWindows)
			break;
	}
	computeWeights();
}

bool Af::setLensPosition(double dioptres, int *hwpos)
{
	bool changed = false;

	if (mode_ == AfModeManual) {
		LOG(RPiAf, Debug) << "setLensPosition: " << dioptres;
		ftarget_ = cfg_.map.domain().clip(dioptres);
		changed = !(initted_ && fsmooth_ == ftarget_);
		updateLensPosition();
	}

	if (hwpos)
		*hwpos = cfg_.map.eval(fsmooth_);

	return changed;
}

std::optional<double> Af::getLensPosition() const
{
	/*
	 * \todo We ought to perform some precise timing here to determine
	 * the current lens position.
	 */
	return initted_ ? std::optional<double>(fsmooth_) : std::nullopt;
}

void Af::cancelScan()
{
	LOG(RPiAf, Debug) << "cancelScan";
	if (mode_ == AfModeAuto)
		goIdle();
}

void Af::triggerScan()
{
	LOG(RPiAf, Debug) << "triggerScan";
	if (mode_ == AfModeAuto && scanState_ == ScanState::Idle)
		scanState_ = ScanState::Trigger;
}

void Af::setMode(AfAlgorithm::AfMode mode)
{
	LOG(RPiAf, Debug) << "setMode: " << (unsigned)mode;
	if (mode_ != mode) {
		mode_ = mode;
		pauseFlag_ = false;
		if (mode == AfModeContinuous)
			scanState_ = ScanState::Trigger;
		else if (mode != AfModeAuto || scanState_ < ScanState::Coarse)
			goIdle();
	}
}

AfAlgorithm::AfMode Af::getMode() const
{
	return mode_;
}

void Af::pause(AfAlgorithm::AfPause pause)
{
	LOG(RPiAf, Debug) << "pause: " << (unsigned)pause;
	if (mode_ == AfModeContinuous) {
		if (pause == AfPauseResume && pauseFlag_) {
			pauseFlag_ = false;
			if (scanState_ < ScanState::Coarse)
				scanState_ = ScanState::Trigger;
		} else if (pause != AfPauseResume && !pauseFlag_) {
			pauseFlag_ = true;
			if (pause == AfPauseImmediate || scanState_ < ScanState::Coarse)
				goIdle();
		}
	}
}

// Register algorithm with the system.
static Algorithm *create(Controller *controller)
{
	return (Algorithm *)new Af(controller);
}
static RegisterAlgorithm reg(NAME, &create);

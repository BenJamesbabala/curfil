#include "hyperopt.h"

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/math/distributions/students_t.hpp>
#include <cmath>
#include <tbb/mutex.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_for.h>

#include "export.hpp"
#include "ndarray_ops.h"
#include "predict.h"
#include "utils.h"

namespace curfil {

bool continueSearching(const std::vector<double>& currentBestLosses,
        const std::vector<double>& currentRunLosses) {

    static const size_t MIN_SAMPLES = 3;

    if (currentBestLosses.empty()) {
        INFO("current best is empty. continue searching");
        return true;
    }

    // we want a minimum of three samples
    if (currentRunLosses.size() < MIN_SAMPLES) {
        INFO("too few elements in current run: " << currentRunLosses.size()
                << ". continue searching");
        return true;
    }

    boost::accumulators::accumulator_set<double,
            boost::accumulators::features<boost::accumulators::tag::max,
                    boost::accumulators::tag::mean,
                    boost::accumulators::tag::variance> > acc1;

    boost::accumulators::accumulator_set<double,
            boost::accumulators::features<boost::accumulators::tag::min,
                    boost::accumulators::tag::mean,
                    boost::accumulators::tag::variance> > acc2;

    acc1 = std::for_each(currentBestLosses.begin(), currentBestLosses.end(), acc1);
    acc2 = std::for_each(currentRunLosses.begin(), currentRunLosses.end(), acc2);

    double worstCurrentBest = boost::accumulators::max(acc1);
    double bestOfCurrentLosses = boost::accumulators::min(acc2);

    // continue if any loss is at least as good as the worst loss of the currently best
    if (bestOfCurrentLosses <= worstCurrentBest) {
        INFO("best run of current parameters is at least as good as the worst run with the best parameters: "
                << bestOfCurrentLosses << " <= " << worstCurrentBest
                << ". continue searching");
        return true;
    }

    // http://www.boost.org/doc/libs/1_53_0/libs/math/doc/sf_and_dist/html/math_toolkit/dist/stat_tut/weg/st_eg/two_sample_students_t.html

    double Sm1 = boost::accumulators::mean(acc1);                 // Sm1 = Sample 1 Mean.
    double variance1 = boost::accumulators::variance(acc1);
    double Sd1 = std::sqrt(variance1);                            // Sd1 = Sample 1 Standard Deviation.
    unsigned Sn1 = currentBestLosses.size();                      // Sn1 = Sample 1 Size.

    double Sm2 = boost::accumulators::mean(acc2);                 // Sm2 = Sample 2 Mean.
    double variance2 = boost::accumulators::variance(acc2);
    double Sd2 = std::sqrt(variance2);                            // Sd2 = Sample 2 Standard Deviation.
    unsigned Sn2 = currentRunLosses.size();                       // Sn2 = Sample 2 Size.
    double alpha = 0.005;                                         // alpha = Significance Level.

    // Degrees of freedom:
    double v = Sn1 + Sn2 - 2;
    // Pooled variance:
    double sp = sqrt(((Sn1 - 1) * Sd1 * Sd1 + (Sn2 - 1) * Sd2 * Sd2) / v);
    // t-statistic:
    double t_stat = (Sm1 - Sm2) / (sp * sqrt(1.0 / Sn1 + 1.0 / Sn2));

    boost::math::students_t dist(v);
    double q = boost::math::cdf(boost::math::complement(dist, fabs(t_stat)));

    INFO("Sm1: " << Sm1 << ", Sd1: " << Sd1 << ", Sn1: " << Sn1);
    INFO("Sm2: " << Sm2 << ", Sd2: " << Sd2 << ", Sn2: " << Sn2);
    INFO("q: " << q << ", alpha: " << alpha);

    // continue if we reject hypothesis that sample 1 mean loss is less than sample 2 mean loss. then we do not know and need to continue searching
    return (q > alpha);
}

double Result::getLoss() const {
    double accuracy;
    switch (lossFunctionType) {
        case LossFunctionType::CLASS_ACCURACY:
            accuracy = getClassAccuracy();
            break;
        case LossFunctionType::CLASS_ACCURACY_NO_VOID:
            accuracy = getClassAccuracyNoVoid();
            break;
        case LossFunctionType::PIXEL_ACCURACY:
            accuracy = getPixelAccuracy();
            break;
        case LossFunctionType::PIXEL_ACCURACY_NO_BACKGROUND:
            accuracy = getPixelAccuracyNoBackground();
            break;
        case LossFunctionType::PIXEL_ACCURACY_NO_VOID:
            accuracy = getPixelAccuracyNoVoid();
            break;
        default:
            throw std::runtime_error("unknown type of loss function");
    }
    assertProbability(accuracy);
    return 1.0 - accuracy;
}

mongo::BSONObj Result::toBSON() const {

    mongo::BSONObjBuilder o(256);
    o << "randomSeed" << getRandomSeed();
    o << "loss" << getLoss();
    o << "classAccuracy" << getClassAccuracy();
    o << "classAccuracyNoVoid" << getClassAccuracyNoVoid();
    o << "pixelAccuracy" << getPixelAccuracy();
    o << "pixelAccuracyNoBackground" << getPixelAccuracyNoBackground();
    o << "pixelAccuracyNoVoid" << getPixelAccuracyNoVoid();

    mongo::BSONArrayBuilder confusionMatrixArray;
    for (size_t label = 0; label < confusionMatrix.getNumClasses(); label++) {
        mongo::BSONArrayBuilder confusionMatrixRow;
        for (size_t prediction = 0; prediction < confusionMatrix.getNumClasses(); prediction++) {
            double probability = confusionMatrix(label, prediction);
            confusionMatrixRow.append(probability);
        }
        confusionMatrixArray.append(confusionMatrixRow.arr());
    }
    o << "confusionMatrix" << confusionMatrixArray.arr();

    return o.obj();
}

LossFunctionType HyperoptClient::parseLossFunction(const std::string& lossFunctionString) {
    if (lossFunctionString == "classAccuracy") {
        return LossFunctionType::CLASS_ACCURACY;
    } else if (lossFunctionString == "classAccuracyNoVoid") {
        return LossFunctionType::CLASS_ACCURACY_NO_VOID;
    }
    else if (lossFunctionString == "pixelAccuracy") {
        return LossFunctionType::PIXEL_ACCURACY;
    }
    else if (lossFunctionString == "pixelAccuracyNoBackground") {
        return LossFunctionType::PIXEL_ACCURACY_NO_BACKGROUND;
    }
    else if (lossFunctionString == "pixelAccuracyNoVoid") {
        return LossFunctionType::PIXEL_ACCURACY_NO_VOID;
    } else {
        throw std::runtime_error(std::string("unknown type of loss function: ") + lossFunctionString);
    }
}

const Result HyperoptClient::test(const RandomTreeImageEnsemble& randomForest,
        const std::vector<LabeledRGBDImage>& testImages) {

    tbb::mutex totalMutex;
    utils::Average averageAccuracy;
    utils::Average averageAccuracyNoBackground;
    utils::Average averageAccuracyNoVoid;

    std::vector<int> indices(testImages.size(), 0);
    for (size_t i = 0; i < testImages.size(); i++) {
        indices[i] = i;
    }

    const LabelType numClasses = randomForest.getNumClasses();

    INFO("testing " << testImages.size() << " images with " << static_cast<int>(numClasses) << " classes");

    ConfusionMatrix totalConfusionMatrix(numClasses);

    tbb::parallel_for_each(indices.begin(), indices.end(), [&](const int& i) {
        const RGBDImage* image = testImages[i].getRGBDImage();
        const LabelImage* groundTruth = testImages[i].getLabelImage();
        LabelImage prediction(image->getWidth(), image->getHeight());

        randomForest.test(image, prediction);

        tbb::mutex::scoped_lock lock(totalMutex);

        ConfusionMatrix confusionMatrix(numClasses);

        double accuracy = calculateAccuracy(prediction, groundTruth, confusionMatrix);
        double accuracyNoBackground = calculateAccuracyNoBackground(prediction, groundTruth);
        double accuracyNoVoid = calculateAccuracyNoVoid(prediction, groundTruth);

        totalConfusionMatrix += confusionMatrix;

        averageAccuracy.addValue(accuracy);
        averageAccuracyNoBackground.addValue(accuracyNoBackground);
        averageAccuracyNoVoid.addValue(accuracyNoVoid);
    });

    tbb::mutex::scoped_lock lock(totalMutex);
    double accuracy = averageAccuracy.getAverage();
    double accuracyNoBackground = averageAccuracyNoBackground.getAverage();
    double accuracyNoVoid = averageAccuracyNoVoid.getAverage();

    INFO("accuracy: " << accuracy
            << ", accuracyNoBg: " << accuracyNoBackground
            << ", accuracyNoVoid: " << accuracyNoVoid);

    return Result(totalConfusionMatrix, accuracy, accuracyNoBackground, accuracyNoVoid, lossFunction);
}

RandomTreeImageEnsemble HyperoptClient::train(size_t trees,
        const TrainingConfiguration& configuration,
        const std::vector<LabeledRGBDImage>& trainImages) {

    INFO("trees: " << trees);
    INFO(configuration);

    // Train

    RandomTreeImageEnsemble randomForest(trees, configuration);

    static const bool trainTreesSequentially = true;

    utils::Timer trainTimer;
    randomForest.train(trainImages, trainTreesSequentially);
    trainTimer.stop();

    INFO("training took " << trainTimer.format(2) <<
            " (" << std::setprecision(3) << trainTimer.getSeconds() / 60.0 << " min)");

    mongo::BSONObjBuilder featureBuilder(64);
    for (const auto& featureCount : randomForest.countFeatures()) {
        featureBuilder.append(featureCount.first, static_cast<int>(featureCount.second));
    }

    mongo::BSONObjBuilder builder(64);
    builder.append("training_time_millis", trainTimer.getMilliseconds());
    builder.append("featureCounts", featureBuilder.obj());

    log(1, builder.obj());

    return randomForest;
}

double HyperoptClient::measureTrueLoss(unsigned int numTrees, TrainingConfiguration configuration,
        const double histogramBias, double& variance) {

    boost::accumulators::accumulator_set<double,
            boost::accumulators::features<boost::accumulators::tag::mean,
                    boost::accumulators::tag::variance> > acc;

    static const size_t TRUE_LOSS_RUNS = 2;

    INFO("measuring true loss");

    Sampler sampler(randomSeed, 1, 100000);

    for (size_t run = 0; run < TRUE_LOSS_RUNS; run++) {

        INFO("true loss run " << (run + 1) << "/" << TRUE_LOSS_RUNS);

        const int seedOfRun = sampler.getNext();

        configuration.setRandomSeed(seedOfRun);

        RandomTreeImageEnsemble forest = train(numTrees, configuration, allRGBDImages);
        forest.normalizeHistograms(histogramBias);
        Result result = test(forest, allTestImages);

        result.setRandomSeed(seedOfRun);

        INFO(result.getConfusionMatrix());

        log(2, BSON("trueLossRun" << static_cast<int>(run)
                << "result" << result.toBSON()));

        acc(result.getLoss());
    }

    variance = boost::accumulators::variance(acc);

    double trueLoss = boost::accumulators::mean(acc);

    INFO("true loss: " << trueLoss);

    return trueLoss;
}

double HyperoptClient::getAverageLossAndVariance(const std::vector<Result>& results, double& variance) {

    boost::accumulators::accumulator_set<double,
            boost::accumulators::features<boost::accumulators::tag::mean,
                    boost::accumulators::tag::variance> > acc;

    for (const Result& result : results) {
        acc(result.getLoss());
    }

    variance = boost::accumulators::variance(acc);

    return boost::accumulators::mean(acc);
}

void HyperoptClient::randomSplit(const int randomSeed, const double testRatio,
        std::vector<LabeledRGBDImage>& trainImages,
        std::vector<LabeledRGBDImage>& testImages) {

    boost::mt19937 rng(randomSeed);
    boost::uniform_real<> dist(0.0, 1.0);

    trainImages.clear();
    testImages.clear();

    while (testImages.empty() || trainImages.empty()) {
        for (const auto& image : allRGBDImages) {
            double random = dist(rng);
            if (random < testRatio) {
                testImages.push_back(image);
            } else {
                trainImages.push_back(image);
            }
        }
    }

    INFO("random split of " << testRatio <<
            ". train images: " << trainImages.size() <<
            ", test images: " << testImages.size());
}

void HyperoptClient::handle_task(const mongo::BSONObj& task) {
    try {
        INFO("got task object: " << task.toString());

        const unsigned int numTrees = getParameterDouble(task, "numTrees");
        const unsigned int samplesPerImage = getParameterDouble(task, "samplesPerImage");
        const unsigned int featureCount = getParameterDouble(task, "featureCount");
        const unsigned int minSampleCount = getParameterDouble(task, "minSampleCount");
        const int maxDepth = getParameterDouble(task, "maxDepth");
        const uint16_t boxRadius = getParameterDouble(task, "boxRadius");
        const uint16_t regionSize = getParameterDouble(task, "regionSize");
        const uint16_t thresholds = getParameterDouble(task, "thresholds");
        const double histogramBias = getParameterDouble(task, "histogramBias");
        const AccelerationMode accelerationMode = AccelerationMode::GPU_ONLY;

        std::vector<Result> results;

        Sampler sampler(randomSeed, 1, 100000);

        static const int RUNS = 5;
        const double testRatio = 1.0 / RUNS;

        std::vector<double> currentRunLosses;

        for (int run = 0; run < RUNS; run++) {

            INFO("starting run " << (run + 1) << "/" << RUNS);

            const int seedOfRun = sampler.getNext();

            TrainingConfiguration configuration(seedOfRun, samplesPerImage, featureCount, minSampleCount, maxDepth,
                    boxRadius, regionSize, thresholds, numThreads, maxImages, imageCacheSize, maxSamplesPerBatch,
                    accelerationMode, useCIELab, useDepthFilling, deviceIds, subsamplingType, ignoredColors);

            std::vector<LabeledRGBDImage> trainImages;
            std::vector<LabeledRGBDImage> testImages;

            randomSplit(seedOfRun, testRatio, trainImages, testImages);

            mongo::BSONObj msg = BSON("run" << run
                    << "randomSeed" << seedOfRun
                    << "numRGBDImages" << static_cast<int>(trainImages.size())
                    << "numTestImages" << static_cast<int>(testImages.size()));

            log(3, msg);
            checkpoint();

            RandomTreeImageEnsemble forest = train(numTrees, configuration, trainImages);
            forest.normalizeHistograms(histogramBias);
            Result result = test(forest, testImages);
            result.setRandomSeed(seedOfRun);

            results.push_back(result);

            currentRunLosses.push_back(result.getLoss());

            log(3, result.toBSON());
            checkpoint();

            mongo::BSONObj bestTask;
            if (get_best_task(bestTask)) {

                INFO("best task so far: " << bestTask.getObjectField("result").toString());

                std::vector<double> currentBestLosses;

                mongo::BSONElementSet values;
                bestTask.getFieldsDotted("result.results.loss", values);

                for (const mongo::BSONElement& loss : values) {
                    currentBestLosses.push_back(loss.Double());
                }

                if (!continueSearching(currentBestLosses, currentRunLosses)) {
                    log(1, BSON("stopSearching" << true
                            << "run" << run
                            << "currentBestLosses" << currentBestLosses
                            << "currentRunLosses" << currentRunLosses ));
                    INFO("stop searching");
                    break;
                } else {
                    INFO("continue searching");
                }
            } else {
                INFO("no finished task so far");
            }

        }

        double lossVariance;
        double loss = getAverageLossAndVariance(results, lossVariance);

        TrainingConfiguration configuration(randomSeed, samplesPerImage, featureCount, minSampleCount, maxDepth,
                boxRadius, regionSize, thresholds, numThreads, maxImages, imageCacheSize, maxSamplesPerBatch,
                accelerationMode, useCIELab, useDepthFilling, deviceIds, subsamplingType, ignoredColors);

        double trueLossVariance;
        double trueLoss = measureTrueLoss(numTrees, configuration, histogramBias, trueLossVariance);

        mongo::BSONObjBuilder builder(64);

        builder << "status" << "ok";
        builder << "loss" << loss << "loss_variance" << lossVariance;
        builder << "true_loss" << trueLoss << "true_loss_variance" << trueLossVariance;

        std::vector<mongo::BSONObj> resultsDetails;
        for (size_t i = 0; i < results.size(); i++) {
            resultsDetails.push_back(results[i].toBSON());
        }

        builder.append("results", resultsDetails);

        finish(builder.obj(), true);

    } catch (const std::runtime_error& e) {
        ERROR(e.what());
        finish(BSON("status" << "fail" << "why" << e.what()), false);
        throw e;
    }

}

double HyperoptClient::getParameterDouble(const mongo::BSONObj& task, const std::string& field) {
    const mongo::BSONObj vals = task.getObjectField("vals");
    if (!vals.hasField(field.c_str())) {
        throw std::runtime_error(task.toString() + " has no value '" + field + "'");
    }
    const mongo::BSONElement value = vals.getField(field);
    const std::vector<mongo::BSONElement> a = value.Array();
    return a.at(0).Double();
}

void HyperoptClient::run() {

    mongo::BSONObj result;
    if (get_best_task(result)) {
        INFO("best result so far: " << result["result"].toString());
    } else {
        INFO("no best result so far");
    }

    INFO("running client");
    bool log_when_waiting = true;
    while (true) {
        mongo::BSONObj task;
        if (!get_next_task(task)) {
            if (log_when_waiting)
                INFO("no task in queue. waiting...");
            log_when_waiting = false;
            sleep(1);
        } else {
            handle_task(task);
            log_when_waiting = true;
        }
    }
}

}

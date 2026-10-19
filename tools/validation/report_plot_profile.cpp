#include "report_plot.hpp"
#include "workbench_style.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>

// Fixed display-only workload. This does not time propagation, GPU work or presentation.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc != 2 || QFile::exists(QString::fromLocal8Bit(argv[1]))) {
        std::cerr << "Usage: orbitforge_report_plot_profile <new-output-directory>\n";
        return 1;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    if (!QDir().mkpath(output))
        return 1;
    using namespace orbitforge;
    desktop::apply_workbench_style(app);
    constexpr int objects = 12, samples = 20000;
    application::ReportDefinition definition;
    definition.id = "profile";
    definition.name = "Fixed report display workload";
    definition.format.time_scale = application::ReportTimeScale::tai;
    std::vector<results::Column> columns{
        {"object.id", "ID", ""}, {"object.name", "Object", ""}, {"time.epoch", "Time", "epoch"}};
    std::vector<desktop::ReportCurveField> fields;
    for (int field = 0; field < 6; ++field) {
        const auto id = "state." + std::to_string(field);
        const auto name = "Field " + std::to_string(field + 1);
        columns.push_back({id, name, field < 3 ? "m" : "m/s"});
        fields.push_back({static_cast<std::size_t>(field + 3),
                          QString::fromStdString(id),
                          QString::fromStdString(name),
                          {}});
    }
    std::vector<results::Row> rows;
    rows.reserve(objects * samples);
    for (int object = 0; object < objects; ++object)
        for (int sample = 0; sample < samples; ++sample) {
            results::Row row{std::string("object-") + std::to_string(object),
                             std::string("Satellite ") + std::to_string(object + 1),
                             core::Epoch{static_cast<std::int64_t>(sample) * 1'000'000'000}};
            for (int field = 0; field < 6; ++field)
                row.emplace_back(1000 * std::sin(sample * 0.0008 + object * .2 + field * .4) +
                                 (sample == 10001 ? 2500 : 0));
            rows.push_back(std::move(row));
        }
    auto result = std::make_shared<runtime::ReportResult>(
        runtime::ReportResult{definition, results::Table{std::move(columns), std::move(rows)}});
    result->last = core::Epoch{static_cast<std::int64_t>(samples - 1) * 1'000'000'000};
    desktop::ReportPlot plot;
    plot.resize(1200, 650);
    plot.show();
    app.processEvents();
    QJsonObject metrics;
    const auto measure = [&](const char *name, int repeats, const auto &operation) {
        std::vector<double> timings;
        for (int i = 0; i < repeats + 5; ++i) {
            QElapsedTimer clock;
            clock.start();
            operation(i);
            const double elapsed = clock.nsecsElapsed() * 1e-6;
            if (i >= 5)
                timings.push_back(elapsed);
        }
        std::sort(timings.begin(), timings.end());
        metrics[name] = QJsonObject{
            {"count", repeats},
            {"mean", std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size()},
            {"p50", timings[(timings.size() - 1) / 2]},
            {"p95", timings[(timings.size() - 1) * 95 / 100]}};
    };
    measure("rebuild_ms", 10, [&](int) { plot.set_result(result, fields); });
    measure("zoom_ms", 60, [&](int i) { plot.zoom(i % 2 ? 1.25 : .8); });
    plot.reset_view();
    QImage image;
    measure("hover_render_ms", 60, [&](int i) {
        const QPointF position(250 + i * 8 % 700, 180);
        QMouseEvent move(QEvent::MouseMove, position, plot.mapToGlobal(position), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&plot, &move);
        image = plot.grab().toImage();
    });
    plot.reset_view();
    const auto full = plot.image(1);
    if (!full.save(output + "/full.png"))
        return 1;
    plot.zoom(.2);
    if (!plot.image(1).save(output + "/zoom.png"))
        return 1;
    QJsonObject report{
        {"scope", "GUI CPU only; fixed synthetic report; no propagation or presentation timing"},
        {"objects", objects},
        {"samples_per_object", samples},
        {"fields", 6},
        {"width", plot.width()},
        {"height", plot.height()},
        {"device_pixel_ratio", plot.devicePixelRatioF()},
        {"metrics", metrics}};
    const auto bytes = QJsonDocument(report).toJson();
    QFile file(output + "/profile.json");
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return 1;
    std::cout << bytes.toStdString();
}

#include "SeventvPaints.hpp"

#include "common/NetworkRequest.hpp"
#include "common/NetworkResult.hpp"
#include "common/Outcome.hpp"
#include "messages/Image.hpp"
#include "providers/seventv/paints/LinearGradientPaint.hpp"
#include "providers/seventv/paints/PaintDropShadow.hpp"
#include "providers/seventv/paints/RadialGradientPaint.hpp"
#include "providers/seventv/paints/UrlPaint.hpp"

#include <QUrlQuery>

namespace {
using namespace chatterino;

QColor rgbaToQColor(const uint32_t color)
{
    auto red = (int)((color >> 24) & 0xFF);
    auto green = (int)((color >> 16) & 0xFF);
    auto blue = (int)((color >> 8) & 0xFF);
    auto alpha = (int)(color & 0xFF);

    return {red, green, blue, alpha};
}

std::optional<QColor> parsePaintColor(const QJsonValue &color)
{
    if (color.isNull())
    {
        return std::nullopt;
    }

    return rgbaToQColor(color.toInt());
}

QGradientStops parsePaintStops(const QJsonArray &stops)
{
    QGradientStops parsedStops;
    double lastStop = -1;

    for (const auto &stop : stops)
    {
        const auto stopObject = stop.toObject();

        const auto rgbaColor = stopObject["color"].toInt();
        auto position = stopObject["at"].toDouble();

        // HACK: qt does not support hard edges in gradients like css does
        // Setting a different color at the same position twice just overwrites
        // the previous color. So we have to shift the second point slightly
        // ahead, simulating an actual hard edge
        if (position == lastStop)
        {
            position += 0.0000001;
        }

        lastStop = position;
        parsedStops.append(QGradientStop(position, rgbaToQColor(rgbaColor)));
    }

    return parsedStops;
}

std::vector<PaintDropShadow> parseDropShadows(const QJsonArray &dropShadows)
{
    std::vector<PaintDropShadow> parsedDropShadows;

    for (const auto &shadow : dropShadows)
    {
        const auto shadowObject = shadow.toObject();

        const auto xOffset = shadowObject["x_offset"].toDouble();
        const auto yOffset = shadowObject["y_offset"].toDouble();
        const auto radius = shadowObject["radius"].toDouble();
        const auto rgbaColor = shadowObject["color"].toInt();

        parsedDropShadows.emplace_back(xOffset, yOffset, radius,
                                       rgbaToQColor(rgbaColor));
    }

    return parsedDropShadows;
}

std::optional<std::shared_ptr<Paint>> parsePaint(const QJsonObject &paintJson)
{
    const QString name = paintJson["name"].toString();
    const QString id = paintJson["id"].toString();

    const auto color = parsePaintColor(paintJson["color"]);
    const bool repeat = paintJson["repeat"].toBool();
    const float angle = (float)paintJson["angle"].toDouble();

    const QGradientStops stops = parsePaintStops(paintJson["stops"].toArray());

    const auto shadows = parseDropShadows(paintJson["drop_shadows"].toArray());

    const QString function = paintJson["function"].toString();
    if (function == "LINEAR_GRADIENT" || function == "linear-gradient")
    {
        return std::make_shared<LinearGradientPaint>(name, id, color, stops,
                                                     repeat, angle, shadows);
    }

    if (function == "RADIAL_GRADIENT" || function == "radial-gradient")
    {
        return std::make_shared<RadialGradientPaint>(name, id, stops, repeat,
                                                     shadows);
    }

    if (function == "URL" || function == "url")
    {
        const QString url = paintJson["image_url"].toString();
        const ImagePtr image = Image::fromUrl({url}, 1);
        if (image == nullptr)
        {
            return std::nullopt;
        }

        return std::make_shared<UrlPaint>(name, id, image, shadows);
    }

    return std::nullopt;
}

}  // namespace

namespace chatterino {

void SeventvPaints::initialize(Settings & /*settings*/, Paths & /*paths*/)
{
    this->loadSeventvPaints();
}

std::optional<std::shared_ptr<Paint>> SeventvPaints::getPaint(
    const QString &userName) const
{
    std::shared_lock lock(this->mutex_);

    const auto it = this->paintMap_.find(userName);
    if (it != this->paintMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void SeventvPaints::addPaint(const QJsonObject &paintJson)
{
    const auto paintID = paintJson["id"].toString();

    std::unique_lock lock(this->mutex_);

    if (this->knownPaints_.find(paintID) != this->knownPaints_.end())
    {
        return;
    }

    std::optional<std::shared_ptr<Paint>> paint = parsePaint(paintJson);
    if (!paint)
    {
        return;
    }

    this->knownPaints_[paintID] = *paint;
}

void SeventvPaints::assignPaintToUser(const QString &paintID,
                                      const UserName &userName)
{
    std::unique_lock lock(this->mutex_);

    const auto paintIt = this->knownPaints_.find(paintID);
    if (paintIt != this->knownPaints_.end())
    {
        this->paintMap_[userName.string] = paintIt->second;
    }
}

void SeventvPaints::clearPaintFromUser(const QString &paintID,
                                       const UserName &userName)
{
    std::unique_lock lock(this->mutex_);

    const auto it = this->paintMap_.find(userName.string);
    if (it != this->paintMap_.end() && it->second->id == paintID)
    {
        this->paintMap_.erase(userName.string);
    }
}

void SeventvPaints::loadSeventvPaints()
{
    static QUrl url("https://7tv.io/v2/cosmetics");

    static QUrlQuery urlQuery;
    // valid user_identifier values: "object_id", "twitch_id", "login"
    urlQuery.addQueryItem("user_identifier", "login");

    url.setQuery(urlQuery);

    NetworkRequest(url)
        .onSuccess([this](const auto &result) -> Outcome {
            auto root = result.parseJson();

            std::unique_lock lock(this->mutex_);

            for (const auto paintValueRef : root.value("paints").toArray())
            {
                const auto paintJson = paintValueRef.toObject();

                std::optional<std::shared_ptr<Paint>> paint =
                    parsePaint(paintJson);
                if (!paint)
                {
                    continue;
                }

                this->knownPaints_[paintJson["id"].toString()] = *paint;

                for (const auto userJson : paintJson["users"].toArray())
                {
                    this->paintMap_[userJson.toString()] = *paint;
                }
            }

            return Success;
        })
        .execute();
}

}  // namespace chatterino

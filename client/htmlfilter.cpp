#include "htmlfilter.h"

#include "logging_categories.h"

#include <Quotient/util.h>

#include <QtGui/QTextDocument>
#include <QtGui/QFontDatabase>

#include <QtCore/QRegularExpression>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QXmlStreamWriter>
#include <QtCore/QStringBuilder>
#include <QtCore/QDebug>

#include <stack>

using namespace std;

namespace HtmlFilter {

enum Mode : unsigned char { QtToMatrix, MatrixToQt, GenericToQt };

class Processor {
public:
    [[nodiscard]] static Result process(QString html, Mode mode,
                                        QuaternionRoom* context,
                                        Options options = Default);

private:
    const Mode mode;
    const Options options;
    QuaternionRoom* const context;
    QXmlStreamWriter& writer;
    int errorPos = -1;
    QString errorString {};

    Processor(Mode mode, Options options, QuaternionRoom* context,
              QXmlStreamWriter& writer)
        : mode(mode), options(options), context(context), writer(writer)
    {}
    void runOn(const QString& html);

    using rewrite_t = vector<pair<QString, QXmlStreamAttributes>>;

    [[nodiscard]] rewrite_t filterTag(QStringView tag,
                                      QXmlStreamAttributes attributes);
    void filterText(QString& text);
};

static const QStringView permittedTags[] = {
    u"font",   u"del",        u"h1", u"h2", u"h3",      u"h4",     u"h5",
    u"h6",     u"blockquote", u"p",  u"a",  u"ul",      u"ol",     u"sup",
    u"sub",    u"li",         u"b",  u"i",  u"u",       u"strong", u"em",
    u"strike", u"code",       u"hr", u"br", u"div",     u"table",  u"thead",
    u"tbody",  u"tr",         u"th", u"td", u"caption", u"pre",    u"span",
    u"img",    u"mx-reply"
};

struct PassList {
    QStringView tag;
    vector<QStringView> allowedAttrs;
};

// See filterTag() on special processing of commented out tags/attributes
static const PassList passLists[] = {
    { u"a", { u"name", u"target", /* "href" - only from permittedSchemes */ } },
    { u"img", { u"width", u"height", u"alt", u"title", u"data-mx-emoticon"
               /* "src" - only 'mxc:' */ } },
    { u"ol", { u"start" } },
    { u"font", { u"color", u"data-mx-color", u"data-mx-bg-color" } },
    { u"span", { u"color", u"data-mx-color", u"data-mx-bg-color" } }
    //, { "code", { "class" /* must start with 'language-' */ } } // Special case
};

static QStringView const permittedSchemes[] {
    u"http:", u"https:", u"ftp:", u"mailto:", u"magnet:", u"matrix:"
};

static const auto htmlColorAttr = u"color";
static const auto htmlStyleAttr = u"style";
static const auto mxColorAttr = u"data-mx-color";
static const auto mxBgColorAttr = u"data-mx-bg-color";

[[nodiscard]] QString mergeMarkdown(const QString& html)
{
    // This code intends to merge user-entered Markdown+HTML markup
    // (HTML-escaped at this point) into HTML exported by QTextDocument.
    // Unfortunately, Markdown engine of QTextDocument is not dealing well
    // with ampersands and &-escaped HTML entities inside HTML tags:
    // see https://bugreports.qt.io/browse/QTBUG-91222
    // Instead, Processor::runOn() splits segments between HTML tags and
    // filterText() treats each of them as Markdown individually.
    QXmlStreamReader reader(html);
    QString mdWithHtml;
    QXmlStreamWriter writer(&mdWithHtml);
    while (reader.readNext() != QXmlStreamReader::StartElement
               || reader.qualifiedName() != u"p")
        if (reader.atEnd()) {
            Q_ASSERT_X(false, __FUNCTION__, "Malformed Qt markup");
            qCCritical(HTMLFILTER)
                << "The passed text doesn't seem to come from QTextDocument";
            return {};
        }

    int depth = 1; // Count <p> just entered
    while (!reader.atEnd()) {
        // Minimal validation, just pipe things through
        // decoding what needs decoding
        const auto tokenType = reader.readNext();
        switch (tokenType) {
        case QXmlStreamReader::Characters:
        case QXmlStreamReader::EntityReference: {
            auto text = reader.text().toString();
            if (depth > 1)
                break;

            // Flush the writer's buffer before side-writing
            writer.writeCharacters({});
            mdWithHtml += text; // Append text as is
            continue;
        }

        case QXmlStreamReader::StartElement:
            ++depth;
            if (reader.qualifiedName() != u"p")
                break;
            // Convert <p> elements except the first one
            // to Markdown paragraph breaks
            writer.writeCharacters("\n\n");
            continue;
        case QXmlStreamReader::EndElement:
            --depth;
            if (reader.qualifiedName() == u"p")
                continue; // See above in StartElement
            break;
        case QXmlStreamReader::Comment:
            continue; // Just drop comments
        default:
            qCWarning(HTMLFILTER) << "Unexpected token, type" << tokenType;
        }
        if (depth < 0) {
            Q_ASSERT(tokenType == QXmlStreamReader::EndElement
                     && reader.qualifiedName() == u"body");
            break;
        }
        writer.writeCurrentToken(reader);
    }
    writer.writeEndElement();
    QTextDocument doc;
    doc.setMarkdown(mdWithHtml);
    return doc.toHtml();
}

[[nodiscard]] inline bool isTagNameTerminator(QChar c)
{
    return c.isSpace() || c == '/' || c == '>';
}

/*! \brief Massage user HTML to look more like XHTML
 *
 * Since Qt doesn't have an HTML parser (outside of QTextDocument)
 * Processor::runOn() uses QXmlStreamReader instead, and it's quite picky
 * about properly closed tags and escaped ampersands. Processor::process()
 * deals with the ampersands; this helper further tries to convert the passed
 * HTML to something more XHTML-like, so that the XML reader doesn't choke on,
 * e.g., unclosed `br` or `img` tags and minimised HTML attributes. It also
 * filters away tags that are not compliant with Matrix specification, where
 * appropriate.
 */
[[nodiscard]] Result preprocess(QString html, Mode mode, Options options)
{
    Q_ASSERT(mode != QtToMatrix);
    bool isFragment = options.testFlag(Fragment) || mode == MatrixToQt;
    bool inHead = false;
    for (auto pos = html.indexOf('<'); pos != -1; pos = html.indexOf('<', pos)) {
        const auto tagNamePos = pos + 1 + (html[pos + 1] == '/');
        const QStringView uncheckedHtml = QStringView(html).mid(tagNamePos);
        static const QLatin1String commentOpen("!--");
        static const QLatin1String commentClose("-->");
        if (uncheckedHtml.startsWith(commentOpen)) { // Skip comments
            pos = html.indexOf(commentClose, tagNamePos + commentOpen.size())
                  + commentClose.size();
            continue;
        }
        // Look ahead to detect stray < and escape it
        auto gtPos = html.indexOf('>', tagNamePos);
        decltype(pos) nextLtPos;
        if (gtPos == tagNamePos /* <> or </> */ || gtPos == -1 /* no more > */
            || ((nextLtPos = html.indexOf('<', tagNamePos)) != -1
                && nextLtPos < gtPos) /* there's another < before > */) {
            static const auto to = QStringLiteral("&lt;");
            html.replace(pos, 1, to);
            pos += to.size(); // Put pos after the escaped sequence
            continue;
        }
        if (uncheckedHtml.startsWith(u"head>", Qt::CaseInsensitive)) {
            if (mode == MatrixToQt) {
                // Matrix spec doesn't allow <head>; report if it occurs in
                // user input (Validate is on) or remove the whole header if
                // it comes from the wire (Validate is off).
                if (options.testFlag(Validate))
                    return { {},
                             pos,
                             "<head> elements are not allowed in Matrix" };
                static const QLatin1String HeadEnd("</head>");
                const auto headEndPos =
                    html.indexOf(HeadEnd, tagNamePos, Qt::CaseInsensitive);
                html.remove(pos, headEndPos - pos + HeadEnd.size());
                continue;
            }
            Q_ASSERT(mode == GenericToQt);
            inHead = html[pos + 1] != '/'; // Track header entry and exit
            if (!inHead) { // Just exited, </head>
                pos = gtPos + 1;
                continue;
            }
        }

        const auto tagEndIt = find_if(uncheckedHtml.cbegin(),
                                      uncheckedHtml.cend(), isTagNameTerminator);
        const auto tag =
            uncheckedHtml.left(int(tagEndIt - uncheckedHtml.cbegin()))
                .toString()
                .toLower();
        // <head> contents are necessary to apply styles but obviously
        // neither `head` nor tags inside of it are in permittedTags;
        // however, minimised attributes still have to be handled everywhere
        // and <meta> tags should be closed
        if (mode == GenericToQt && (tag == u"html" || tag == u"body")) {
            // Only in generic mode, allow <html> and <body>
            pos += tagNamePos + tag.size() + 1;
            isFragment = false;
            continue;
        }
        if (!inHead) {
            // Check if it's a valid (opening or closing) tag allowed in Matrix
            const auto tagIt =
                find(cbegin(permittedTags), cend(permittedTags), tag);
            if (tagIt == cend(permittedTags)) {
                // Invalid tag or non-tag - either remove the abusing piece
                // or stop and report
                if (options.testFlag(Validate))
                    return { {},
                             pos,
                             "Non-tag or disallowed tag: "
                                 % uncheckedHtml.left(gtPos - tagNamePos) };

                html.remove(pos, gtPos - pos + 1);
                continue;
            }
        }

        // Treat minimised attributes
        // (https://www.w3.org/TR/xhtml1/diffs.html#h-4.5)

        // There's no simple way to replace all occurences within
        // a string segment; so just go through the segment and insert
        // `=''` after minimized attributes.
        // This is not the place to _filter_ allowed/disallowed attributes -
        // filtering is left for filterTag()
        static const QRegularExpression MinAttrRE {
            R"(([^[:space:]>/"'=]+)\s*(=\s*([^[:space:]>/"']|"[^"]*"|'[^']*')+)?)"
        };
        pos = tagNamePos + tag.size();
        QRegularExpressionMatch m;
        while ((m = MinAttrRE.match(html, pos)).hasMatch()
               && m.capturedEnd(1) < gtPos) {
            pos = m.capturedEnd();
            if (m.captured(2).isEmpty()) {
                static const auto attrValue = QString("=''");
                html.insert(m.capturedEnd(1), attrValue);
                gtPos += attrValue.size() - 1;
                pos += attrValue.size() - 1;
            }
        }
        // Make sure empty elements are properly closed
        static const QRegularExpression EmptyElementRE {
            "^img|[hb]r|meta$", QRegularExpression::CaseInsensitiveOption
        };
        if (html[gtPos - 1] != '/' && EmptyElementRE.match(tag).hasMatch()) {
            html.insert(gtPos, '/');
            ++gtPos;
        }
        pos = gtPos + 1;
        Q_ASSERT(pos > 0);
    }
    // Wrap in a no-op tag to make the text look like valid XML if it's
    // a fragment (always the case when HTML comes from a homeserver, and
    // possibly with generic HTML).
    if (isFragment)
        html = "<span>" % html % "</span>";
    // Discard characters behind the last tag (LibreOffice attaches \n\0, e.g.)
    html.truncate(html.lastIndexOf('>') + 1);
    return { html };
}

Result Processor::process(QString html, Mode mode, QuaternionRoom* context,
                          Options options)
{
    // Since Qt doesn't have an HTML parser (outside of QTextDocument; and
    // the one in QTextDocument is opinionated and not configurable)
    // Processor::runOn() uses QXmlStreamReader instead. Being an XML parser,
    // this class is quite picky about properly closed tags and escaped
    // ampersands. Before passing to runOn(), the following code tries to bring
    // the passed HTML to something more XHTML-like, so that the XML parser
    // doesn't choke on things HTML-but-not-XML. In QtToMatrix mode the only
    // such thing is unescaped ampersands in attributes (especially `href`),
    // since QTextDocument::toHtml() produces (otherwise) valid XHTML. In other
    // modes no such assumption can be made so an attempt is taken to close
    // elements that are normally empty (`br`, `hr` and `img`), turn minimised
    // attributes to their full interpretations (`disabled -> disabled=''`)
    // and remove things that are obvious non-tags around unescaped `<`
    // characters.

    // 1. Escape ampersands outside of character entities
    static const QRegularExpression freestandingAmps{ QStringLiteral(
        "&(?!(#[0-9]+|#x[0-9a-fA-F]+|[[:alpha:]_][-[:alnum:]_:.]*);)") };
    html.replace(freestandingAmps, QStringLiteral("&amp;"));

    if (mode == QtToMatrix) {
        if (options.testFlag(ConvertMarkdown)) {
            // The processor handles Markdown in chunks between HTML tags;
            // <br /> breaks character sequences that are otherwise valid
            // Markdown, leading to issues with, e.g., lists.
            html.replace(QStringLiteral("<br />"), QStringLiteral("\n"));
#if 0
            html = mergeMarkdown(html);
            if (html.isEmpty())
                return { "", 0, "This markup doesn't seem to be sourced from Qt" };
            options &= ~ConvertMarkdown;
#endif
        }
    } else {
        auto r = preprocess(html, mode, options);
        if (r.errorPos != -1)
            return r;
        html = r.filteredHtml;
    }

    QString resultHtml;
    QXmlStreamWriter writer(&resultHtml);
    writer.setAutoFormatting(false);
    Processor p { mode, options, context, writer };
    p.runOn(html);
    return { resultHtml.trimmed(), p.errorPos, p.errorString };
}

QString toMatrixHtml(const QString& qtMarkup, QuaternionRoom* context,
                     Options options)
{
    // Validation of HTML emitted by Qt doesn't make much sense
    Q_ASSERT(!options.testFlag(Validate));
    const auto& result =
        Processor::process(qtMarkup, QtToMatrix, context, options);
    Q_ASSERT(result.errorPos == -1);
    return result.filteredHtml;
}

Result fromMatrixHtml(const QString& matrixHtml, QuaternionRoom* context,
                      Options options)
{
    // Matrix HTML body should never be treated as Markdown
    Q_ASSERT(!options.testFlag(ConvertMarkdown));
    auto result = Processor::process(matrixHtml, MatrixToQt, context, options);
    if (result.errorPos == -1) {
        // Make sure to preserve whitespace sequences
        result.filteredHtml = "<span style=\"white-space: pre-wrap\">"
                              % result.filteredHtml % "</span>";
    }
    return result;
}

Result fromLocalHtml(const QString& html,
                     QuaternionRoom* context, Options options)
{
    return Processor::process(html, GenericToQt, context, options);
}

namespace {
    class EntityResolver : public QXmlStreamEntityResolver {
    public:
        using QXmlStreamEntityResolver::QXmlStreamEntityResolver;
        Q_DISABLE_COPY_MOVE(EntityResolver)

    private:
        QString resolveUndeclaredEntity(const QString& name) override
        {
            return name == u"nbsp" ? QStringLiteral("\xa0") : QString();
        }
    };
    EntityResolver entityResolver;
}

void Processor::runOn(const QString &html)
{
    QXmlStreamReader reader(html);
    reader.setEntityResolver(&entityResolver);

    /// The entry in the (outer) stack corresponds to each level in the source
    /// document; the (inner) stack in each entry records open elements in the
    /// target document.
    using open_tags_t = stack<QString, vector<QString>>;
    stack<open_tags_t, vector<open_tags_t>> tagsStack;

    /// Accumulates characters and resolved entry references until the next
    /// tag (opening or closing); used to linkify (or process Markdown in)
    /// text parts.
    QString textBuffer;
    int bodyOffset = 0;
    bool firstElement = true, inAnchor = false;
    while (!reader.atEnd()) {
        const auto tokenType = reader.readNext();
        if (bodyOffset == -1) // See below in 'case StartElement:'
            bodyOffset = reader.characterOffset();

        if (!textBuffer.isEmpty() && !reader.isCharacters()
            && !reader.isEntityReference())
            filterText(textBuffer);

        switch (tokenType) {
        case QXmlStreamReader::StartElement: {
            const auto& tagName = reader.qualifiedName();
            if (tagsStack.empty()) {
                // These tags are invalid anywhere deeper, and we don't even
                // care to put them to tagsStack
                if (tagName == u"html") {
                    if (mode == GenericToQt)
                        writer.writeCurrentToken(reader);
                    break; // Otherwise, just ignore, get to the content inside
                }
                if (tagName == u"head") {
                    // <head> is only needed for Qt to import HTML more
                    // accurately, and entirely uninteresting in other modes
                    if (mode != GenericToQt) {
                        reader.skipCurrentElement();
                        break;
                    }
                    // Copy through the whole <head> element - having
                    // QXmlStreamWriter::writeCurrentElement() would help
                    // but there's none such
                    do {
                        writer.writeCurrentToken(reader);
                        const auto nextTokenType = reader.readNext();
                        if (nextTokenType == QXmlStreamReader::EndElement
                            && reader.qualifiedName() == u"head") {
                            writer.writeCurrentToken(reader);
                            break;
                        }
                    } while (!reader.atEnd());
                    continue;
                }
                if (tagName == u"body") {
                    if (mode == GenericToQt)
                        writer.writeCurrentToken(reader);
                    // Except importing HTML into QTextDocument, skip just like
                    // <html> but record the position for error reporting
                    // (FIXME: this position is still not exactly related to
                    // the original text...)
                    bodyOffset = -1; // See the end of the while loop
                    break;
                }
            }

            const auto& attrs = reader.attributes();
            if (find_if(attrs.cbegin(), attrs.cend(),
                        [](const auto& a) {
                            return a.qualifiedName() == u"style"
                                   && a.value().contains(
                                       u"-qt-paragraph-type:empty");
                        })
                != attrs.cend()) {
                reader.skipCurrentElement();
                continue; // Hidden text block, just skip it
            }

            tagsStack.emplace();
            if (tagsStack.size() > 100)
                qCCritical(HTMLFILTER)
                    << "CS API spec limits HTML tags depth at 100";

            // Qt hardcodes the link style in a `<span>` under `<a>`.
            // This breaks the looks on the receiving side if the sender
            // uses a different style of links from that of the receiver.
            // Since Qt decorates links when importing HTML anyway, we
            // don't lose anything if we just strip away this span tag.
            if (mode != MatrixToQt && inAnchor && textBuffer.isEmpty()
                && tagName == u"span" && attrs.size() == 1
                && attrs.front().qualifiedName() == u"style")
                continue; // inAnchor == true ==> firstElement == false

            // Skip the first top-level <p> and replace further top-level
            // `<p>...</p>` with `<br/>...` - kinda controversial but
            // there's no cleaner way to get rid of the single top-level <p>
            // generated by Qt without assuming that it's the only <p>
            // spanning the whole body (copy-pasting rich text from other
            // editors can bring several legitimate paragraphs of text,
            // e.g.). This is also a very special case where a converted tag
            // is immediately closed, unlike the one in the source text;
            // which is why it's checked here rather than in filterTag().
            if (mode == QtToMatrix && tagName == u"p"
                && tagsStack.size() == 1 /* top-level, just emplaced */) {
                if (firstElement)
                    continue; // Skip unsetting firstElement at the loop end
                writer.writeEmptyElement("br");
                break;
            }
            if (tagName != u"mx-reply"
                || (firstElement && !options.testFlag(Fragment))) {
                // ^ The spec only allows `<mx-reply>` at the very beginning
                // and it's not supposed to be in the user input
                const auto& rewrite = filterTag(tagName, attrs);
                for (const auto& [rewrittenTag, rewrittenAttrs]: rewrite) {
                    tagsStack.top().push(rewrittenTag);
                    writer.writeStartElement(rewrittenTag);
                    writer.writeAttributes(rewrittenAttrs);
                    if (rewrittenTag == "a")
                        inAnchor = true;
                }
            }
            break;
        }
        case QXmlStreamReader::Characters:
        case QXmlStreamReader::EntityReference: {
            if (firstElement && mode == QtToMatrix) {
                // Remove the line break Qt inserts after <body> because it
                // adds an unnecessary whitespace in the HTML context and
                // an unnecessary line break in the Markdown context.
                if (reader.text().startsWith('\n')) {
                    textBuffer += reader.text().mid(1);
                    continue; // Maintain firstElement
                }
            }
            // Outside of links, defer writing until the next non-character,
            // non-entity reference token in order to pass the whole text
            // piece to filterText() with all entity references resolved.
            if (!inAnchor && !options.testFlag(Fragment))
                textBuffer += reader.text();
            else
                writer.writeCurrentToken(reader);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (tagsStack.empty()) {
                const auto& tagName = reader.qualifiedName();
                if (tagName != u"body" && tagName != u"html")
                    qCWarning(HTMLFILTER)
                        << "filterHtml(): empty tags stack, skipping"
                        << ('/' + tagName.toString());
                break;
            }
            // Close as many elements as were opened in case StartElement
            for (auto& t = tagsStack.top(); !t.empty(); t.pop()) {
                writer.writeEndElement();
                if (t.top() == "a")
                    inAnchor = false;
            }
            tagsStack.pop();
            break;
        case QXmlStreamReader::EndDocument:
            if (!tagsStack.empty())
                qCWarning(HTMLFILTER).noquote().nospace()
                    << ": Not all HTML tags closed";
            if (mode == GenericToQt)
                writer.writeEndDocument(); // </body></html>
            break;
        case QXmlStreamReader::NoToken:
            Q_ASSERT(reader.tokenType() != QXmlStreamReader::NoToken /*false*/);
            break;
        case QXmlStreamReader::Invalid: {
            errorPos = reader.characterOffset() - bodyOffset;
            errorString = reader.errorString();
            qCCritical(HTMLFILTER) << "Invalid XHTML:" << html;
            qCCritical(HTMLFILTER).nospace()
                << "Error at char " << errorPos << ": " << errorString;
            const auto remainder =
                QStringView(html).mid(reader.characterOffset());
            qCCritical(HTMLFILTER).nospace()
                << "Buffer at error: " << remainder << ", "
                << html.size() - reader.characterOffset()
                << " character(s) remaining";
            break;
        }
        case QXmlStreamReader::Comment:
        case QXmlStreamReader::StartDocument:
        case QXmlStreamReader::DTD:
        case QXmlStreamReader::ProcessingInstruction:
            continue; // All these should not affect firstElement state
        }
        // Unset first element once encountered non-whitespace under `<body>`
        // NB: all `continue` statements above intentionally bypass this
        firstElement &= (bodyOffset <= 0 || reader.isWhitespace());
    }
}

template <size_t Len>
inline QStringView cssValue(QStringView css,
                            const char16_t (&propertyNameWithColon)[Len])
{
    return css.startsWith(propertyNameWithColon)
               ? css.mid(Len - 1).trimmed()
               : QStringView();
}

Processor::rewrite_t Processor::filterTag(QStringView tag,
                                          QXmlStreamAttributes attributes)
{
    if (mode == MatrixToQt) {
        if (tag == u"del" || tag == u"strike") { // Qt doesn't support these...
            QXmlStreamAttributes attrs;
            attrs.append("style", "text-decoration:line-through");
            return { { "font", std::move(attrs) } };
        }
        if (tag == u"mx-reply")
            return { { "div", {} } }; // The spec says that mx-reply is HTML div
        // If `mx-reply` is encountered on the way to the wire, just pass it
    }

    rewrite_t rewrite { { tag.toString(), {} } };
    if (tag == u"code" && mode != GenericToQt) { // Special case
        copy_if(attributes.begin(), attributes.end(),
                back_inserter(rewrite.back().second), [](const auto& a) {
                    return a.qualifiedName() == u"class"
                           && a.value().startsWith(u"language-");
                });
        return rewrite;
    }

    if (find(begin(permittedTags), end(permittedTags), tag) == end(permittedTags))
        return {}; // The tag is not allowed

    const auto it =
        find_if(begin(passLists), end(passLists),
                [&tag](const auto& passCard) { return passCard.tag == tag; });
    if (it == end(passLists))
        return rewrite; // Drop all attributes, pass the tag

    /// Find the first element in the rewrite that would accept color
    /// attributes (`font` and, only in Matrix HTML, `span`),
    /// and add the passed attribute to it
    const auto& addColorAttr = [&rewrite, this](QStringView attrName,
                                                QStringView attrValue) {
        auto it = find_if(rewrite.begin(), rewrite.end(),
                          [this](const rewrite_t::value_type& element) {
                              return element.first == "font"
                                     || (mode == QtToMatrix
                                         && element.first == "span");
                          });
        if (it == rewrite.end())
            it = rewrite.insert(rewrite.end(), { QStringLiteral("font"), {} });
        it->second.append(attrName.toString(), attrValue.toString());
    };

    const auto& passList = it->allowedAttrs;
    for (auto&& a: attributes) {
        const auto aName = a.qualifiedName();
        const auto aValue = a.value();
        // Attribute conversions between Matrix and Qt subsets; generic HTML
        // is treated as possibly-Matrix
        if (mode != QtToMatrix) {
            if (aName == mxColorAttr) {
                addColorAttr(htmlColorAttr, aValue.toString());
                continue;
            }
            if (aName == mxBgColorAttr) {
                rewrite.front().second.append(QString::fromUtf16(htmlStyleAttr),
                                              "background-color:"
                                                  + aValue.toString());
                continue;
            }
        } else {
            if (aName == htmlStyleAttr) {
                // 'style' attribute is not allowed in Matrix; convert
                // everything possible to tags and other attributes
                const auto& cssProperties = aValue.split(';');
                for (auto p: cssProperties) {
                    p = p.trimmed();
                    if (p.isEmpty())
                        continue;
                    if (const auto& v = cssValue(p, u"color:"); !v.isEmpty()) {
                        addColorAttr(mxColorAttr, v);
                    } else if (const auto& v = cssValue(p, u"background-color:");
                               !v.isEmpty())
                        addColorAttr(mxBgColorAttr, v);
                    else if (const auto& v = cssValue(p, u"font-weight:");
                             v == u"bold" || v == u"bolder" || v.toFloat() > 500)
                        rewrite.emplace_back().first = "b";
                    else if (const auto& v = cssValue(p, u"font-style:");
                             v == u"italic" || v.startsWith(u"oblique"))
                        rewrite.emplace_back().first = "i";
                    else if (const auto& v = cssValue(p, u"text-decoration:");
                             v.contains(u"line-through"))
                        rewrite.emplace_back().first = QStringLiteral("del");
                    else {
                        const auto& fontFamilies =
                            cssValue(p, u"font-family:").split(',');
                        for (auto ff: fontFamilies) {
                            ff = ff.trimmed();
                            if (ff.isEmpty())
                                continue;
                            if (ff[0] == '\'' || ff[0] == '"')
                                ff = ff.mid(1, ff.length() - 2);
                            if (QFontDatabase().isFixedPitch(ff.toString())) {
                                rewrite.emplace_back().first = "code";
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if (aName == htmlColorAttr)
                addColorAttr(mxColorAttr, aValue); // Add to 'color'
        }

        // Generic filtering for attributes
        if ((mode == GenericToQt
             && (aName == htmlStyleAttr || aName == u"class" || aName == u"id"))
            || (tag == u"a" && aName == u"href"
                && any_of(begin(permittedSchemes), end(permittedSchemes),
                          [&aValue](QStringView s) {
                              return aValue.startsWith(s);
                          }))
            || (tag == u"img" && aName == u"src" && aValue.startsWith(u"mxc:"))
            || find(passList.begin(), passList.end(), a.qualifiedName())
                   != passList.end())
            rewrite.front().second.push_back(std::move(a));
    } // for (a: attributes)

    // Remove the original <font> or <span> if they end up without attributes
    // since without attributes they are no-op
    if (!rewrite.empty()
        && (rewrite.front().first == "font" || rewrite.front().first == "span")
        && rewrite.front().second.empty())
        rewrite.erase(rewrite.begin());

    return rewrite;
}

void Processor::filterText(QString& text)
{
    if (text.isEmpty())
        return;

    if (options.testFlag(ConvertMarkdown)) {
        // Protect leading/trailing whitespaces (Markdown disregards them);
        // specific string doesn't matter as long as it isn't whitespace itself,
        // doesn't have special meaning in Markdown and doesn't occur in
        // the HTML boilerplate that QTextDocument generates.
        static const QLatin1String Marker { "$$" };
        const bool hasLeadingWhitespace = text.cbegin()->isSpace();
        if (hasLeadingWhitespace)
            text.prepend(Marker);
        const bool hasTrailingWhitespace = (text.cend() - 1)->isSpace();
        if (hasTrailingWhitespace)
            text.append(Marker);
        const auto markerCount = text.count(Marker); // For self-check

#ifndef QTBUG_92445_FIXED
        // Protect list items from https://bugreports.qt.io/browse/QTBUG-92445
        // (see also https://spec.commonmark.org/0.29/#list-items)
        static const auto ReOptions = QRegularExpression::MultilineOption;
        static const QRegularExpression //
            UlRE("^( *[-+*] {1,4})(?=[^ ])", ReOptions),
            OlRE("^( *[0-9]{1,9}+[.)] {1,4})(?=[^ ])", ReOptions);
        static const QLatin1String UlMarker("@@ul@@"), OlMarker("@@ol@@");
        text.replace(UlRE, "\\1" % UlMarker);
        text.replace(OlRE, "\\1" % OlMarker);
        const auto markerCountOl = text.count(OlMarker);
        const auto markerCountUl = text.count(UlMarker);
#endif

        // Convert Markdown to HTML
        QTextDocument doc;
        doc.setMarkdown(text, QTextDocument::MarkdownNoHTML);
        text = doc.toHtml();

        // Delete protection characters, now buried inside HTML
#ifndef QTBUG_92445_FIXED
        Q_ASSERT(text.count(OlMarker) == markerCountOl);
        Q_ASSERT(text.count(UlMarker) == markerCountUl);
        // After HTML conversion, list markers end up being after HTML tags
        text.replace(QRegularExpression('>' % OlMarker), ">");
        text.replace(QRegularExpression('>' % UlMarker), ">");
#endif

        Q_ASSERT(text.count(Marker) == markerCount);
        if (hasLeadingWhitespace)
            text.remove(text.indexOf(Marker), Marker.size());
        if (hasTrailingWhitespace)
            text.remove(text.lastIndexOf(Marker), Marker.size());
    } else {
        text = text.toHtmlEscaped(); // The reader unescaped it
        Quotient::linkifyUrls(text);
        text = "<span>" % text % "</span>";
    }
    // Re-process this piece of text as HTML but dump text snippets as they are,
    // without recursing into filterText() again
    Processor(mode, Fragment, context, writer).runOn(text);

    text.clear();
}

} // namespace HtmlFilter

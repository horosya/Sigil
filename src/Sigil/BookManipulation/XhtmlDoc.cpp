/**
**  Copyright (C) 2015 Kevin B. Hendricks Stratford, ON, Canada 
**  Copyright (C) 2012 John Schember <john@nachtimwald.com>
**  Copyright (C) 2012 Dave Heiland
**  Copyright (C) 2009, 2010, 2011  Strahinja Markovic  <strahinja.markovic@gmail.com>
**
**  This file is part of Sigil.
**
**  Sigil is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  Sigil is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with Sigil.  If not, see <http://www.gnu.org/licenses/>.
**
*************************************************************************/

#include <memory>
#include <string>

#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
// XercesExtensions
#include <LocationAwareDOMParser.h>
#include <NodeLocationInfo.h>
#include <XmlUtils.h>

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QXmlStreamReader>
#include <QtWebKitWidgets/QWebFrame>
#include <QtWebKitWidgets/QWebPage>
#include <QtXml/QXmlInputSource>
#include <QtXml/QXmlSimpleReader>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QFileInfo>

#include "BookManipulation/CleanSource.h"
#include "BookManipulation/XercesCppUse.h"
#include "BookManipulation/XhtmlDoc.h"
#include "Misc/Utility.h"
#include "sigil_constants.h"
#include "sigil_exception.h"

const QStringList BLOCK_LEVEL_TAGS = QStringList() << "address" << "blockquote" << "center" << "dir" << "div" <<
                                     "dl" << "fieldset" << "form" << "h1" << "h2" << "h3" <<
                                     "h4" << "h5" << "h6" << "hr" << "isindex" << "menu" <<
                                     "noframes" << "noscript" << "ol" << "p" << "pre" <<
                                     "table" << "ul" << "body";

const QStringList IMAGE_TAGS = QStringList() << "img" << "image";
const QStringList VIDEO_TAGS = QStringList() << "video";
const QStringList AUDIO_TAGS = QStringList() << "audio";

static const QStringList INVALID_ID_TAGS = QStringList() << "base" << "head" << "meta" << "param" << "script" << "style" << "title";
static const QStringList SKIP_ID_TAGS = QStringList() << "html" << "#document" << "body";
const QStringList ID_TAGS = QStringList() << BLOCK_LEVEL_TAGS <<
                            "dd" << "dt" << "li" << "tbody" << "td" << "tfoot" <<
                            "th" << "thead" << "tr" << "a" << "abbr" <<
                            "acronym" << "address" << "b" << "big" <<
                            "caption" << "center" << "cite" << "code" << "font" <<
                            "label" << "i" << "pre" << "small" << "span" <<
                            "strike" << "strong" << "sub" << "sup" << "u";
const QStringList ANCHOR_TAGS = QStringList() << "a";
const QStringList SRC_TAGS = QStringList() << "link" << "img";


const int XML_DECLARATION_SEARCH_PREFIX_SIZE = 150;
static const int XML_CUSTOM_ENTITY_SEARCH_PREFIX_SIZE = 500;
static const QString ENTITY_SEARCH = "<!ENTITY\\s+(\\w+)\\s+\"([^\"]+)\">";
static const QString URL_ATTRIBUTE_SEARCH = ":.*(url\\s*\\([^\\)]+\\))";

const QString BREAK_TAG_SEARCH  = "(<div>\\s*)?<hr\\s*class\\s*=\\s*\"[^\"]*(sigil_split_marker|sigilChapterBreak)[^\"]*\"\\s*/>(\\s*</div>)?";

// Resolves custom ENTITY declarations
QString XhtmlDoc::ResolveCustomEntities(const QString &source)
{
    QString search_prefix = source.left(XML_CUSTOM_ENTITY_SEARCH_PREFIX_SIZE);

    if (!search_prefix.contains("<!ENTITY")) {
        return source;
    }

    QString new_source = source;
    QRegularExpression entity_search(ENTITY_SEARCH);
    QHash<QString, QString> entities;
    int main_index = 0;

    // Catch all custom entity declarations...
    while (true) {
        QRegularExpressionMatch match = entity_search.match(new_source, main_index);
        if (!match.hasMatch()) {
            break;
        }

        main_index = match.capturedStart();
        if (main_index == -1) {
            break;
        }

        entities["&" + match.captured(1) + ";"] = match.captured(2);
        // Erase the entity declaration
        new_source.replace(match.captured(), "");
    }

    // ...and now replace all occurrences
    foreach(QString key, entities.keys()) {
        new_source.replace(key, entities[ key ]);
    }
    // Clean up what's left of the custom entity declaration field
    new_source.replace(QRegularExpression("\\[\\s*\\]>"), "");
    return new_source;
}


// Returns a list of XMLElements representing all
// the elements of the specified tag name
// in the head section of the provided XHTML source code
QList<XhtmlDoc::XMLElement> XhtmlDoc::GetTagsInHead(const QString &source, const QString &tag_name)
{
    // TODO: how about replacing uses of this function
    // with XPath expressions? Profile for speed.
    QXmlStreamReader reader(source);
    bool in_head = false;
    QList<XMLElement> matching_elements;

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement()) {
            if (reader.name() == "head" || reader.name() == "HEAD") {
                in_head = true;
            } else if (in_head && reader.name() == tag_name) {
                matching_elements.append(CreateXMLElement(reader));
            }
        } else if (reader.isEndElement() &&
                   (reader.name() == "head" || reader.name() == "HEAD")
                  ) {
            break;
        }
    }

    if (reader.hasError()) {
        std::string msg = reader.errorString().toStdString() + ": " + QString::number(reader.lineNumber()).toStdString() + ": " + QString::number(reader.columnNumber()).toStdString();
        throw (ErrorParsingXml(msg));
    }

    return matching_elements;
}


// Returns a list of XMLElements representing all
// the elements of the specified tag name
// in the entire document of the provided XHTML source code
QList<XhtmlDoc::XMLElement> XhtmlDoc::GetTagsInDocument(const QString &source, const QString &tag_name)
{
    // TODO: how about replacing uses of this function
    // with XPath expressions? Profile for speed.
    QXmlStreamReader reader(source);
    QList<XMLElement> matching_elements;

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement() &&
            reader.name() == tag_name) {
            matching_elements.append(CreateXMLElement(reader));
        }
    }

    if (reader.hasError()) {
        std::string msg = reader.errorString().toStdString() + ": " + QString::number(reader.lineNumber()).toStdString() + ": " + QString::number(reader.columnNumber()).toStdString();
        throw(msg);
    }

    return matching_elements;
}


QList<xc::DOMNode *> XhtmlDoc::GetNodeChildren(const xc::DOMNode &node)
{
    xc::DOMNodeList *children = node.getChildNodes();
    int num_children = children->getLength();
    QList<xc::DOMNode *> qtchildren;

    for (int i = 0; i < num_children; ++i) {
        qtchildren.append(children->item(i));
    }

    return qtchildren;
}


QHash<QString, QString> XhtmlDoc::GetNodeAttributes(const xc::DOMNode &node)
{
    QHash<QString, QString> attributes_hash;
    xc::DOMNamedNodeMap *attributes = node.getAttributes();

    if (!attributes) {
        return attributes_hash;
    }

    for (uint i = 0; i < attributes->getLength(); ++i) {
        xc::DOMAttr &attribute = *static_cast<xc::DOMAttr *>(attributes->item(i));
        attributes_hash[ GetAttributeName(attribute) ] = XtoQ(attribute.getValue());
    }

    return attributes_hash;
}


QList<QString> XhtmlDoc::GetAllDescendantClasses(const QString & source)
{
    GumboInterface gi = GumboInterface(source);
    QList<GumboNode*> nodes = gi.get_all_nodes_with_attribute(QString("class"));
    QStringList classes;
    foreach(GumboNode * node, nodes) {
        QString element_name = QString::fromStdString(gi.get_tag_name(node));
        GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "class");
        if (attr) {
            QString class_values = QString::fromUtf8(attr->value);
            foreach(QString class_name, class_values.split(" ")) {
                classes.append(element_name + "." + class_name);
            }
        }
    }
    return classes;
}

QList<QString> XhtmlDoc::GetAllDescendantStyleUrls(const QString & source)
{
    GumboInterface gi = GumboInterface(source);
    QList<GumboNode*> nodes = gi.get_all_nodes_with_attribute(QString("style"));
    QStringList styles;
    foreach(GumboNode * node, nodes) {
        GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "style");
        if (attr) {
            QString style_value = QString::fromUtf8(attr->value);
            QRegularExpression url_search(URL_ATTRIBUTE_SEARCH);
            QRegularExpressionMatch match = url_search.match(style_value);
            if (match.hasMatch()) {
                styles.append(match.captured(1));
            }
        }
    }
    return styles;
}


QList<QString> XhtmlDoc::GetAllDescendantIDs(const QString & source)
{
    GumboInterface gi = GumboInterface(source);
    QList<GumboNode*> nodes = gi.get_all_nodes_with_attribute(QString("id"));
    nodes.append(gi.get_all_nodes_with_attribute(QString("name")));
    QStringList IDs;
    foreach(GumboNode * node, nodes) {
        QString element_name = QString::fromStdString(gi.get_tag_name(node));
        GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "id");
        if (attr) {
            IDs.append(QString::fromUtf8(attr->value));
        } else {
            // This is supporting legacy html of <a name="xxx"> (deprecated).
            // Make sure we don't return names of other elements like <meta> tags.
            if (element_name == "a") {
                attr = gumbo_get_attribute(&node->v.element.attributes, "name");
                IDs.append(QString::fromUtf8(attr->value));
            }
        }
    }
    return IDs;
}

QList<QString> XhtmlDoc::GetAllDescendantHrefs(const QString & source)
{
    GumboInterface gi = GumboInterface(source);
    QList<GumboNode*> nodes = gi.get_all_nodes_with_attribute(QString("href"));
    QStringList hrefs;
    foreach(GumboNode * node, nodes) {
        QString element_name = QString::fromStdString(gi.get_tag_name(node));
        GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "href");
        if (attr) {
            hrefs.append(QString::fromUtf8(attr->value));
        }
    }
    return hrefs;
}


std::shared_ptr<xc::DOMDocument> XhtmlDoc::LoadTextIntoDocument(const QString &source)
{
    XercesExt::LocationAwareDOMParser parser;
    // This scanner ignores schemas
    parser.useScanner(xc::XMLUni::fgDGXMLScanner);
    parser.setValidationScheme(xc::AbstractDOMParser::Val_Never);
    parser.useCachedGrammarInParse(true);
    parser.setLoadExternalDTD(true);
    parser.setDoNamespaces(true);
    xc::MemBufInputSource xhtml_dtd(XHTML_ENTITIES_DTD, XHTML_ENTITIES_DTD_LEN, XHTML_ENTITIES_DTD_ID);
    parser.loadGrammar(xhtml_dtd, xc::Grammar::DTDGrammarType, true);
    xc::MemBufInputSource ncx_dtd(NCX_2005_1_DTD, NCX_2005_1_DTD_LEN, NCX_2005_1_DTD_ID);
    parser.loadGrammar(ncx_dtd, xc::Grammar::DTDGrammarType, true);
    QString prepared_source = PrepareSourceForXerces(source);
    // We use source.count() * 2 because count returns
    // the number of QChars, which are 2 bytes long
    xc::MemBufInputSource input(
        reinterpret_cast<const XMLByte *>(prepared_source.utf16()),
        prepared_source.count() * 2,
        "empty");
    XMLCh UTF16[] = { xc::chLatin_U, xc::chLatin_T, xc::chLatin_F, xc::chDigit_1, xc::chDigit_6, xc::chNull };
    input.setEncoding(UTF16);
    parser.parse(input);
    return RaiiWrapDocument(parser.adoptDocument());
}


std::shared_ptr<xc::DOMDocument> XhtmlDoc::RaiiWrapDocument(xc::DOMDocument *document)
{
    return std::shared_ptr<xc::DOMDocument>(document, XercesExt::XercesDeallocator<xc::DOMDocument>);
}


int XhtmlDoc::NodeLineNumber(const xc::DOMNode &node)
{
    return XercesExt::GetNearestNodeLocationInfo(node).LineNumber;
}


int XhtmlDoc::NodeColumnNumber(const xc::DOMNode &node)
{
    return XercesExt::GetNearestNodeLocationInfo(node).ColumnNumber;
}


XhtmlDoc::WellFormedError XhtmlDoc::WellFormedErrorForSource(const QString &source)
{
    GumboInterface gi = GumboInterface(source);
    QList<GumboWellFormedError> results = gi.error_check();
    if (!results.isEmpty()) {
        XhtmlDoc::WellFormedError error;
        error.line    = results.at(0).line;
        error.column  = results.at(0).column;
        error.message = QString(results.at(0).message);
        return error;
    }
    return XhtmlDoc::WellFormedError();
}


bool XhtmlDoc::IsDataWellFormed(const QString &data)
{
    XhtmlDoc::WellFormedError error = XhtmlDoc::WellFormedErrorForSource(data);
    return error.line == -1;
}


// Accepts a string with HTML and returns the text
// in that HTML fragment. For instance:
//   <h1>Hello <b>Qt</b>&nbsp;this is great</h1>
// returns
//   Hello Qt this is great
QString XhtmlDoc::GetTextInHtml(const QString &source)
{
    QWebPage page;
    page.mainFrame()->setHtml(source);
    return page.mainFrame()->toPlainText();
}


// Resolves HTML entities in the provided string.
// For instance:
//    Bonnie &amp; Clyde
// returns
//    Bonnie & Clyde
QString XhtmlDoc::ResolveHTMLEntities(const QString &text)
{
    // Faking some HTML... this is the easiest way to do it
    QString newsource = "<div>" + text + "</div>";
    return GetTextInHtml(newsource);
}


// A tree node class without a children() function...
// appallingly stupid, isn't it?
QList<QWebElement> XhtmlDoc::QWebElementChildren(const QWebElement &element)
{
    QList<QWebElement> children;
    const QWebElement &first_child = element.firstChild();

    if (!first_child.isNull()) {
        children.append(first_child);
    }

    QWebElement next_sibling = first_child.nextSibling();

    while (!next_sibling.isNull()) {
        children.append(next_sibling);
        next_sibling = next_sibling.nextSibling();
    }

    return children;
}


QStringList XhtmlDoc::GetSGFSectionSplits(const QString &source,
        const QString &custom_header)
{
    QRegularExpression body_start_tag(BODY_START);
    QRegularExpressionMatch body_start_tag_match = body_start_tag.match(source);
    QRegularExpression body_end_tag(BODY_END);

    int body_end   = source.indexOf(body_end_tag, 0);
    int main_index = body_start_tag_match.capturedEnd();

    QString header = !custom_header.isEmpty() ? custom_header + "<body>\n" : source.left(main_index);
    QStringList sections;
    QRegularExpression break_tag(BREAK_TAG_SEARCH);

    while (main_index != body_end) {
        QRegularExpressionMatch match = break_tag.match(source, main_index);
        QString body;

        // We search for our HR break tag
        if (match.hasMatch()) {
            // We break up the remainder of the file on the HR tag index if it's found
            int break_index = match.capturedStart();
            body = Utility::Substring(main_index, break_index, source);
            main_index = break_index + match.capturedLength();
        } else {
            // Otherwise, we take the rest of the file
            body = Utility::Substring(main_index, body_end, source);
            main_index = body_end;
        }

        sections.append(header + body + "</body> </html>");
    }

    return sections;
}


QStringList XhtmlDoc::GetLinkedStylesheets(const QString &source)
{
    QList<XhtmlDoc::XMLElement> link_tag_nodes;

    try {
        link_tag_nodes = XhtmlDoc::GetTagsInHead(source, "link");
    } catch (ErrorParsingXml) {
        // Nothing really. If we can't get the CSS style tags,
        // than that's it. No CSS returned.
    }

    QStringList linked_css_paths;
    foreach(XhtmlDoc::XMLElement element, link_tag_nodes) {
        if (element.attributes.contains("type") &&
            (
                (element.attributes.value("type").toLower() == "text/css") ||
                (element.attributes.value("type").toLower() == "text/x-oeb1-css")
            ) &&
            element.attributes.contains("rel") &&
            (element.attributes.value("rel").toLower() == "stylesheet") &&
            element.attributes.contains("href")) {
            linked_css_paths.append(element.attributes.value("href"));
        }
    }
    return linked_css_paths;
}


// Returns the node's "real" name. We don't care
// about namespace prefixes and whatnot.
QString XhtmlDoc::GetNodeName(const xc::DOMNode &node)
{
    QString local_name = XtoQ(node.getLocalName());

    if (local_name.isEmpty()) {
        return XtoQ(node.getNodeName());
    } else {
        return local_name;
    }
}


// TODO: this should be covered by attribute.localName(), no?
QString XhtmlDoc::GetAttributeName(const xc::DOMAttr &attribute)
{
    QString name = XtoQ(attribute.getName());
    int colon_index = name.lastIndexOf(QChar(':'));

    if (colon_index < 0) {
        return name;
    } else {
        return name.mid(colon_index + 1);
    }
}


// Converts a DomNodeList to a regular QList
QList<xc::DOMNode *> XhtmlDoc::ConvertToRegularList(const xc::DOMNodeList &list)
{
    // Since a DomNodeList is "live", we store the count
    // so we don't have to recalculate it every loop iteration
    int count = list.getLength();
    QList<xc::DOMNode *> nodes;

    for (int i = 0; i < count; ++i) {
        nodes.append(list.item(i));
    }

    return nodes;
}


// Returns a list with only the element nodes
QList<xc::DOMNode *> XhtmlDoc::ExtractElements(const xc::DOMNodeList &list)
{
    // Since a DomNodeList is "live", we store the count
    // so we don't have to recalculate it every loop iteration
    int count = list.getLength();
    QList<xc::DOMNode *> element_nodes;

    for (int i = 0; i < count; ++i) {
        xc::DOMNode *node = list.item(i);

        if (node->getNodeType() == xc::DOMNode::ELEMENT_NODE) {
            element_nodes.append(node);
        }
    }

    return element_nodes;
}


// Returns the node's real index in the list
int XhtmlDoc::GetRealIndexInList(const xc::DOMNode &node, const xc::DOMNodeList &list)
{
    // Since a DomNodeList is "live", we store the count
    // so we don't have to recalculate it every loop iteration
    int count = list.getLength();

    for (int i = 0; i < count; ++i) {
        if (list.item(i)->isSameNode(&node)) {
            return i;
        }
    }

    return -1;
}

// Returns the node's "element" index
// (pretending the list is only made up of element nodes).
int XhtmlDoc::GetElementIndexInList(const xc::DOMNode &node, const xc::DOMNodeList &list)
{
    // Since a DomNodeList is "live", we store the count
    // so we don't have to recalculate it every loop iteration
    int count = list.getLength();
    int element_index = 0;

    for (int i = 0; i < count; ++i) {
        if (list.item(i)->isSameNode(&node)) {
            return element_index;
        }

        if (list.item(i)->getNodeType() == xc::DOMNode::ELEMENT_NODE) {
            element_index++;
        }
    }

    return -1;
}

// Returns the index of node in the specified list
// depending on the node type. Text nodes get the "real"
// index, element nodes get the "element" index
// (pretending the list is only made up of element nodes).
int XhtmlDoc::GetCustomIndexInList(const xc::DOMNode &node, const xc::DOMNodeList &list)
{
    if (node.getNodeType() == xc::DOMNode::TEXT_NODE) {
        return GetRealIndexInList(node, list);
    } else {
        return GetElementIndexInList(node, list);
    }
}


// Returns a list of all the "visible" text nodes that are descendants
// of the specified node. "Visible" means we ignore style tags, script tags etc...
QList<xc::DOMNode *> XhtmlDoc::GetVisibleTextNodes(const xc::DOMNode &node)
{
    // TODO: investigate possible parallelization
    // opportunities for this function (profile before and after!)
    if (node.getNodeType() == xc::DOMNode::TEXT_NODE) {
        return QList<xc::DOMNode *>() << const_cast<xc::DOMNode *>(&node);
    } else {
        QString node_name = GetNodeName(node);

        if (node.hasChildNodes()  &&
            node_name != "script" &&
            node_name != "style"
           ) {
            QList<xc::DOMNode *> children = GetNodeChildren(node);
            QList<xc::DOMNode *> text_nodes;

            for (int i = 0; i < children.count(); ++i) {
                text_nodes.append(GetVisibleTextNodes(*children.at(i)));
            }

            return text_nodes;
        }
    }

    return QList<xc::DOMNode *>();
}


// Returns a list of all nodes suitable for "id" element
QList<GumboNode*> XhtmlDoc::GetIDNodes(GumboInterface & gi, GumboNode* node)
{
    QList<GumboNode*> text_nodes = QList<GumboNode*>();
    if (node->type != GUMBO_NODE_ELEMENT) {
        return text_nodes;
    }
    QString node_name = QString::fromStdString(gi.get_tag_name(node));
    GumboVector* children = &node->v.element.children;

    if ((children->length > 0) && (node_name != "head")) {

        if (!INVALID_ID_TAGS.contains(node_name)) {

            if (ID_TAGS.contains(node_name)) {
                text_nodes.append(node);

            } else if (!SKIP_ID_TAGS.contains(node_name)) {
                GumboNode * ancestor_id_node = GetAncestorIDElement(gi, node);

                if (!text_nodes.contains(ancestor_id_node)) {
                    text_nodes.append(ancestor_id_node);
                }
            }

            // Parse children after parent to keep index numbers in order
            for (unsigned int i = 0; i < children->length; ++i) {
                GumboNode* child = static_cast<GumboNode*> (children->data[i]);
                QList<GumboNode *> children_text_nodes = GetIDNodes(gi, child);
                foreach(GumboNode * cnode, children_text_nodes) {
                    if (!text_nodes.contains(cnode)) {
                        text_nodes.append(cnode);
                    }
                }
            }
        }
    }

    return text_nodes;
}


QString XhtmlDoc::GetIDElementText(GumboInterface & gi, GumboNode * node)
{
    QString text;
    if (node->type != GUMBO_NODE_ELEMENT) {
        return text;
    }
    GumboVector* children = &node->v.element.children;

    // Combine all text nodes for this node plus all text for non-ID element children
    for (int i = 0; i < children->length; ++i) {
        GumboNode* child_node = static_cast<GumboNode*>(children->data[i]);
        QString child_node_name = QString::fromStdString(gi.get_tag_name(node));
        if ((child_node->type == GUMBO_NODE_TEXT) || (child_node->type == GUMBO_NODE_WHITESPACE)) {
            text += QString::fromUtf8(child_node->v.text.text);
        } else if (!ID_TAGS.contains(child_node_name)) {
            text += GetIDElementText(gi, child_node);
        }
    }
    return text;
}


// Returns the first block element ancestor of the specified node
xc::DOMNode &XhtmlDoc::GetAncestorBlockElement(const xc::DOMNode &node)
{
    const xc::DOMNode *parent_node = &node;

    while (true) {
        parent_node = parent_node->getParentNode();

        if (BLOCK_LEVEL_TAGS.contains(GetNodeName(*parent_node))) {
            break;
        }
    }

    if (parent_node) {
        return const_cast<xc::DOMNode &>(*parent_node);
    } else {
        return *(node.getOwnerDocument()->getElementsByTagName(QtoX("body"))->item(0));
    }
}

GumboNode * XhtmlDoc::GetAncestorIDElement(GumboInterface & gi, GumboNode * node)
{
    GumboNode * parent_node = node;

    while (true) {
        parent_node = parent_node->parent;
        QString node_name = QString::fromStdString(gi.get_tag_name(parent_node));
        if (ID_TAGS.contains(node_name)) {
            break;
        }
    }
    if (parent_node) {
        return parent_node;
    } else {
        return gi.get_all_nodes_with_tag(GUMBO_TAG_BODY).at(0);
    }
}

QStringList XhtmlDoc::GetPathsToMediaFiles(const QString & source)
{
  QList<GumboTag> tags = QList<GumboTag>() << GIMAGE_TAGS << GVIDEO_TAGS << GAUDIO_TAGS;
  QStringList media_paths = GetAllMediaPathsFromMediaChildren(source, tags);
  // Remove duplicate references
  media_paths.removeDuplicates();
  return media_paths;
}

QStringList XhtmlDoc::GetPathsToStyleFiles(const QString & source)
{
    GumboInterface gi = GumboInterface(source);
    QStringList style_paths;
    QList<GumboNode*> nodes = gi.get_all_nodes_with_tag(GUMBO_TAG_LINK);
    for (int i = 0; i < nodes.count(); ++i) {
        GumboNode* node = nodes.at(i);
        GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "href");
        if (attr) {
            QString relative_path = Utility::URLDecodePath(QString::fromUtf8(attr->value));
            QFileInfo file_info(relative_path);
            if (file_info.suffix().toLower() == "css") {
                style_paths << relative_path;
            }
        }
  }
  style_paths.removeDuplicates();
  return style_paths;
}


QStringList XhtmlDoc::GetAllMediaPathsFromMediaChildren(const QString & source, QList<GumboTag> tags)
{
    GumboInterface gi = GumboInterface(source);
    QStringList media_paths;
    QList<GumboNode*> nodes = gi.get_all_nodes_with_tags(tags);
    for (int i = 0; i < nodes.count(); ++i) {
        GumboNode* node = nodes.at(i);
        GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "src");
        if (!attr) {
            attr = gumbo_get_attribute(&node->v.element.attributes, "xlink:href");
        }
        if (attr) {
            QString relative_path = Utility::URLDecodePath(QString::fromUtf8(attr->value));
            media_paths << relative_path;
        }
    }
    return media_paths;
}


// Accepts a reference to an XML stream reader positioned on an XML element.
// Returns an XMLElement struct with the data in the stream.
XhtmlDoc::XMLElement XhtmlDoc::CreateXMLElement(QXmlStreamReader &reader)
{
    XMLElement element;
    foreach(QXmlStreamAttribute attribute, reader.attributes()) {
        QString attribute_name = attribute.name().toString();

        // We convert non-mixed case attribute names to lower case;
        // simplifies things later on so we for instance don't
        // have to check for both "src" and "SRC".
        if (!Utility::IsMixedCase(attribute_name)) {
            attribute_name = attribute_name.toLower();
        }

        element.attributes[ attribute_name ] = attribute.value().toString();
    }
    element.name = reader.name().toString();
    // Includes child text to avoid error for some XML (e.g. <a><img/></a>)
    element.text = reader.readElementText(QXmlStreamReader::IncludeChildElements);

    return element;
}


QString XhtmlDoc::PrepareSourceForXerces(const QString &source)
{
    QString prefix = source.left(XML_DECLARATION_SEARCH_PREFIX_SIZE);
    QRegularExpression standalone(STANDALONE_ATTRIBUTE);
    QRegularExpressionMatch match = standalone.match(prefix);
    return QString(source).remove(match.capturedStart(), match.capturedLength());
}


// Returns the node identified by the specified ViewEditor element hierarchy
xc::DOMNode *XhtmlDoc::GetNodeFromHierarchy(const xc::DOMDocument &document,
        const QList<ViewEditor::ElementIndex> &hierarchy)
{
    xc::DOMNode *node = document.getElementsByTagName(QtoX("html"))->item(0);
    if (node == NULL) {
        return NULL;
    }
    xc::DOMNode *end_node = NULL;
    xc::DOMNodeList *tmp_node = NULL;

    for (int i = 0; i < hierarchy.count() - 1; ++i) {
        QList<xc::DOMNode *> children;

        tmp_node = node->getChildNodes();
        if (tmp_node != NULL) {
            if (hierarchy[ i + 1 ].name != "#text") {
                children = ExtractElements(*tmp_node);
            } else {
                children = ConvertToRegularList(*tmp_node);
            }
        }

        // If the index is within the range, descend
        if (hierarchy[ i ].index < children.count()) {
            node = children.at(hierarchy[ i ].index);

            if (node) {
                end_node = node;
            } else {
                break;
            }
        }
        // Error handling. The asked-for node cannot be found,
        // so we stop where we are.
        else {
            end_node = node;
            break;
        }
    }

    return end_node;
}

// Creates a ViewEditor element hierarchy from the specified node
QList<ViewEditor::ElementIndex> XhtmlDoc::GetHierarchyFromNode(const xc::DOMNode &node)
{
    xc::DOMNode *html_node = node.getOwnerDocument()->getElementsByTagName(QtoX("html"))->item(0);
    const xc::DOMNode *current_node = &node;
    QList<ViewEditor::ElementIndex> element_list;

    while (current_node != html_node) {
        xc::DOMNode *parent = current_node->getParentNode();
        ViewEditor::ElementIndex element;
        element.name  = GetNodeName(*parent);
        element.index = GetCustomIndexInList(*current_node, *parent->getChildNodes());
        element_list.prepend(element);
        current_node = parent;
    }

    return element_list;
}


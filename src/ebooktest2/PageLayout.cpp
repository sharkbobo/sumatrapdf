/* Copyright 2011-2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "PageLayout.h"
#include "Scoped.h"
#include "StrUtil.h"
#include "HtmlPullParser.h"
#include "GdiPlusUtil.h"

// set consistent mode for our graphics objects so that we get
// the same results when measuring text
void InitGraphicsMode(Graphics *g)
{
    g->SetCompositingQuality(CompositingQualityHighQuality);
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    //g.SetSmoothingMode(SmoothingModeHighQuality);
    g->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g->SetPageUnit(UnitPixel);
}

FontCache::~FontCache()
{
    for (Entry *e = cache.IterStart(); e; e = cache.IterNext()) {
        free(e->name);
        ::delete e->font;
    }
}

Font *FontCache::GetFont(const WCHAR *name, float size, FontStyle style)
{
    Entry f = { (WCHAR *)name, size, style, NULL };
    for (Entry *e = cache.IterStart(); e; e = cache.IterNext()) {
        if (f == *e)
            return e->font;
    }

    f.name = str::Dup(f.name);
    // TODO: handle a failure to create a font. Use fontCache[0] if exists
    // or try to fallback to a known font like Times New Roman
    f.font = ::new Font(name, size, style);
    cache.Append(f);
    return f.font;
}

struct WordInfo {
    const char *s;
    size_t len;

    bool IsNewline() { return len == 1 && *s == '\n'; }
};

class WordsIter {
    const char *s;
    const char *end;
    // modified during the iteration
    WordInfo wi;
    const char *curr;

public:
    WordsIter(const char *s) : s(s), end(s + str::Len(s)), curr(s) { }

    void Reset() { curr = s; }
    WordInfo *Next();
};

// return true if s points to "\n", "\r" or "\r\n"
// and advance s/left to skip it
// We don't want to collapse multiple consequitive newlines into
// one as we want to be able to detect paragraph breaks (i.e. empty
// newlines i.e. a newline following another newline)
static bool IsNewlineSkip(const char *& s, const char *end)
{
    const char *os = s;
    if (s < end && '\r' == *s)
        s++;
    if (s < end && '\n' == *s)
        s++;
    return s != os;
}

// iterates words in a string e.g. "foo bar\n" returns "foo", "bar" and "\n"
// also unifies line endings i.e. "\r" and "\r\n" are turned into a single "\n"
// returning NULL means end of iterations
WordInfo *WordsIter::Next()
{
    while (curr < end && *curr == ' ')
        curr++;
    if (curr == end)
        return NULL;
    assert(*curr != 0);
    if (IsNewlineSkip(curr, end)) {
        wi.len = 1;
        wi.s = "\n";
        return &wi;
    }
    wi.s = curr;
    while (curr < end && !isspace(*curr))
        curr++;
    wi.len = curr - wi.s;
    assert(wi.len > 0);
    return &wi;
}

class PageLayout
{
public:
    PageLayout(FontCache *fontCache);
    ~PageLayout();

    void StartLayout(LayoutInfo* layoutInfo);
    void Process(INewPageObserver *pageObserver, BaseEbookDoc *doc);

private:
    void HandleHtmlTag(HtmlToken *t, BaseEbookDoc *doc);
    void EmitText(HtmlToken *t);

    REAL GetCurrentLineDx();
    void LayoutLeftStartingAt(REAL offX);
    void JustifyLineBoth();
    void JustifyLine(AlignAttr mode);

    void StartNewPage();
    void StartNewLine(bool isParagraphBreak);

    void AddSetFontInstr(Font *font);

    void AddHr();
    void AddWord(WordInfo *wi);
    void AddImage(ImageData2 *data);

    void SetCurrentFont(FontStyle fs);
    void ChangeFont(FontStyle fs, bool isStart);

    DrawInstr *GetInstructionsForCurrentLine(DrawInstr *& endInst) const {
        size_t len = currPage->Count() - currLineInstrOffset;
        DrawInstr *ret = &currPage->drawInstructions.At(currLineInstrOffset);
        endInst = ret + len;
        return ret;
    }

    bool IsCurrentLineEmpty() const {
        return currLineInstrOffset == currPage->Count();
    }

    // constant during layout process
    INewPageObserver *  pageObserver;
    FontCache *         fontCache;
    SizeT<REAL>         pageSize;
    REAL                lineSpacing;
    REAL                spaceDx;
    ScopedMem<WCHAR>    fontName;
    float               fontSize;

    // for measuring text
    Bitmap              bmp;
    Graphics            gfx;

    // temporary state during layout process
    FontStyle           currFontStyle;
    Font *              currFont;

    AlignAttr           currJustification;
    // current position in a page
    PointT<REAL>        curr;
    // number of consecutive newlines
    int                 newLinesCount;

    PageData *          currPage;

    // for iterative parsing
    HtmlPullParser *    htmlParser;

    // current nesting of html tree during html parsing
    Vec<HtmlTag>        tagNesting;

    size_t              currLineInstrOffset;
    WCHAR               buf[512];
};

PageLayout::PageLayout(FontCache *fontCache) : currPage(NULL),
    bmp(1, 1, PixelFormat32bppARGB), gfx(&bmp), fontCache(fontCache),
    pageObserver(NULL)
{
    InitGraphicsMode(&gfx);
}

PageLayout::~PageLayout()
{
    delete currPage;
    delete htmlParser;
}

void PageLayout::SetCurrentFont(FontStyle fs)
{
    currFontStyle = fs;
    currFont = fontCache->GetFont(fontName.Get(), fontSize, fs);
}

// change the current font by adding (if addStyle is true) or removing
// a given font style from current font style
// TODO: it doesn't support the case where the same style is nested
// like "<b>fo<b>oo</b>bar</b>" - "bar" should still be bold but wont
// We would have to maintain counts for each style to do it fully right
void PageLayout::ChangeFont(FontStyle fs, bool addStyle)
{
    FontStyle newFontStyle = currFontStyle;
    if (addStyle)
        newFontStyle = (FontStyle) (newFontStyle | fs);
    else
        newFontStyle = (FontStyle) (newFontStyle & ~fs);

    if (newFontStyle == currFontStyle)
        return; // a no-op
    SetCurrentFont(newFontStyle);
    AddSetFontInstr(currFont);
}

void PageLayout::StartLayout(LayoutInfo* layoutInfo)
{
    pageSize = layoutInfo->pageSize.Convert<REAL>();

    fontName.Set(str::Dup(layoutInfo->fontName));
    fontSize = layoutInfo->fontSize;
    htmlParser = new HtmlPullParser(layoutInfo->htmlStr, layoutInfo->htmlStrLen);

    currJustification = Align_Justify;
    SetCurrentFont(FontStyleRegular);

    CrashIf(currPage);
    lineSpacing = currFont->GetHeight(&gfx);
    // note: this is heuristic that seems to work better than
    //  GetSpaceDx(gfx, currFont) (which seems way too big and is
    // bigger than what Kindle app uses)
    spaceDx = fontSize / 2.5f;
    StartNewPage();
}

void PageLayout::StartNewPage()
{
    if (currPage && pageObserver)
        pageObserver->NewPage(currPage);
    else
        delete currPage;

    currPage = new PageData;
    curr.x = curr.y = 0;
    newLinesCount = 0;
    // instructions for each page need to be self-contained
    // so we have to carry over some state like the current font
    CrashIf(!currFont);
    AddSetFontInstr(currFont);
    currLineInstrOffset = currPage->Count();
}

REAL PageLayout::GetCurrentLineDx()
{
    REAL dx = -spaceDx;
    DrawInstr *end;
    DrawInstr *currInstr = GetInstructionsForCurrentLine(end);
    while (currInstr < end) {
        if (InstrTypeString == currInstr->type) {
            dx += currInstr->bbox.Width;
            dx += spaceDx;
        }
        ++currInstr;
    }
    if (dx < 0)
        dx = 0;
    return dx;
}

void PageLayout::LayoutLeftStartingAt(REAL offX)
{
    curr.x = offX;
    DrawInstr *end;
    DrawInstr *currInstr = GetInstructionsForCurrentLine(end);
    while (currInstr < end) {
        if (InstrTypeString == currInstr->type) {
            // currInstr Width and Height are already set
            currInstr->bbox.X = curr.x;
            currInstr->bbox.Y = curr.y;
            curr.x += (currInstr->bbox.Width + spaceDx);
        }
        ++currInstr;
    }
}

void PageLayout::JustifyLineBoth()
{
    // move all words proportionally to the right so that the
    // spacing remains uniform and the last word touches the
    // right page border
    REAL margin = pageSize.dx - GetCurrentLineDx();
    LayoutLeftStartingAt(0);
    DrawInstr *end;
    DrawInstr *c = GetInstructionsForCurrentLine(end);
    size_t count = end - c;
    REAL extraSpaceDx = count > 1 ? margin / (count - 1) : margin;

    for (size_t n = 1; ++c < end; n++)
        c->bbox.X += n * extraSpaceDx;
}

void PageLayout::JustifyLine(AlignAttr mode)
{
    if (IsCurrentLineEmpty())
        return;

    switch (mode) {
    case Align_Left:
        LayoutLeftStartingAt(0);
        break;
    case Align_Right:
        LayoutLeftStartingAt(pageSize.dx - GetCurrentLineDx());
        break;
    case Align_Center:
        LayoutLeftStartingAt((pageSize.dx - GetCurrentLineDx()) / 2.f);
        break;
    case Align_Justify:
        JustifyLineBoth();
        break;
    default:
        assert(0);
        break;
    }
    currLineInstrOffset = currPage->Count();
}

void PageLayout::StartNewLine(bool isParagraphBreak)
{
    // don't put empty lines at the top of the page
    if (0 == curr.y && IsCurrentLineEmpty())
        return;

    if (isParagraphBreak && Align_Justify == currJustification)
        JustifyLine(Align_Left);
    else
        JustifyLine(currJustification);

    curr.x = 0;
    curr.y += lineSpacing;
    currLineInstrOffset = currPage->Count();
    if (curr.y + lineSpacing > pageSize.dy)
        StartNewPage();
}

void PageLayout::AddSetFontInstr(Font *font)
{
    currPage->Append(DrawInstr::SetFont(font));
}

// add horizontal line (<hr> in html terms)
void PageLayout::AddHr()
{
    // hr creates an implicit paragraph break
    StartNewLine(true);
    curr.x = 0;
    // height of hr is lineSpacing. If drawing it a current position
    // would exceede page bounds, go to another page
    if (curr.y + lineSpacing > pageSize.dy)
        StartNewPage();

    RectF bbox(curr.x, curr.y, pageSize.dx, lineSpacing);
    currPage->Append(DrawInstr::Line(bbox));
    StartNewLine(true);
}

void PageLayout::AddWord(WordInfo *wi)
{
    RectF bbox;
    if (wi->IsNewline()) {
        // a single newline is considered "soft" and ignored
        // two or more consequitive newlines are considered a
        // single paragraph break
        newLinesCount++;
        if (2 == newLinesCount) {
            bool needsTwo = (curr.x != 0);
            StartNewLine(true);
            if (needsTwo)
                StartNewLine(true);
        }
        return;
    }
    newLinesCount = 0;

    // TODO: check if the string contains html entities. If it does,
    // decode the entity, create a copy of decoded string in memory
    // that will packaged with pages information

    size_t strLen = str::Utf8ToWcharBuf(wi->s, wi->len, buf, dimof(buf));
    bbox = MeasureText(&gfx, currFont, buf, strLen);
    // TODO: handle a case where a single word is bigger than the whole
    // line, in which case it must be split into multiple lines
    REAL dx = bbox.Width;
    if (curr.x + dx > pageSize.dx) {
        // start new line if the new text would exceed the line length
        StartNewLine(false);
    }
    bbox.Y = curr.y;
    currPage->Append(DrawInstr::Str(wi->s, wi->len, bbox));
    curr.x += (dx + spaceDx);
}

// add image (<img>)
// TODO: extract desired dimensions from tag
void PageLayout::AddImage(ImageData2 *data)
{
    // TODO: can be done without creating a bitmap, cf. ImagesEngine::SizeFromData
    Bitmap *bmp = BitmapFromData(data->data, data->len);
    if (!bmp)
        return;
    // display all images centered on their own lines, for now
    StartNewLine(false);
    RectF img(0, 0, (REAL)bmp->GetWidth(), (REAL)bmp->GetHeight());
    if (pageSize.dy - curr.y < img.Height / 2) {
        // move overly large images to a new page
        StartNewPage();
    }
    if (img.Width > pageSize.dx || img.Height > pageSize.dy - curr.y) {
        // resize still too large images to fit a page
        REAL factor = min(pageSize.dx / img.Width, (pageSize.dy - curr.y) / img.Height);
        img.Width *= factor;
        img.Height *= factor;
    }
    curr.x += (pageSize.dx - img.Width) / 2;
    img.X = curr.x;
    img.Y = curr.y;
    currPage->Append(DrawInstr::Image(data, img));
    curr.y += img.Height;
    StartNewLine(false);
    delete bmp;
}

void PageLayout::HandleHtmlTag(HtmlToken *t, BaseEbookDoc *doc)
{
    CrashAlwaysIf(!t->IsTag());

    HtmlTag tag = FindTag(t);

    // update the current state of html tree
    if (t->IsStartTag())
        RecordStartTag(&tagNesting, tag);
    else if (t->IsEndTag())
        RecordEndTag(&tagNesting, tag);

    switch (tag) {
    case Tag_P:
        StartNewLine(true);
        currJustification = Align_Justify;
        if (t->IsStartTag()) {
            AttrInfo *attrInfo;
            while ((attrInfo = t->NextAttr())) {
                if (attrInfo->HasName("align"))
                    currJustification = FindAlignAttr(attrInfo->val, attrInfo->valLen);
            }
        }
        break;
    case Tag_Hr:
        AddHr();
        break;
    case Tag_B: case Tag_Strong:
        ChangeFont(FontStyleBold, t->IsStartTag());
        break;
    case Tag_I: case Tag_Em:
        ChangeFont(FontStyleItalic, t->IsStartTag());
        break;
    case Tag_U:
        ChangeFont(FontStyleUnderline, t->IsStartTag());
        break;
    case Tag_Strike:
        ChangeFont(FontStyleStrikeout, t->IsStartTag());
        break;
    case Tag_Pagebreak: case Tag_Mbp_Pagebreak:
        JustifyLine(currJustification);
        StartNewPage();
        break;
    case Tag_Img:
        if (t->IsEndTag())
            /* shouldn't happen, but does */;
        else if (doc) {
            AttrInfo *attrInfo;
            while ((attrInfo = t->NextAttr())) {
                if (attrInfo->HasName("src") || attrInfo->HasName("recindex")) {
                    ScopedMem<char> id(str::DupN(attrInfo->val, attrInfo->valLen));
                    ImageData2 *data = doc->GetImageData(id);
                    if (data)
                        AddImage(data);
                }
            }
        }
        else
            /* TODO: display "missing image box"? */;
        break;
    }
}

void PageLayout::EmitText(HtmlToken *t)
{
    // ignore the content of <style> tags
    if (tagNesting.Find(Tag_Style) != -1)
        return;

    CrashIf(!t->IsText());
    const char *end = t->s + t->sLen;
    const char *curr = t->s;
    SkipWs(curr, end);
    while (curr < end) {
        const char *currStart = curr;
        SkipNonWs(curr, end);
        size_t len = curr - currStart;
        if (len > 0) {
            WordInfo wi = { currStart, len };
            AddWord(&wi);
        }
        SkipWs(curr, end);
    }
}

// Return the next parsed page. Returns NULL if finished parsing.
// For simplicity of implementation, we parse xml text node or
// xml element at a time. This might cause a creation of one
// or more pages, which we send to the caller through pageObserver
void PageLayout::Process(INewPageObserver *pageObserver, BaseEbookDoc *doc)
{
    this->pageObserver = pageObserver;

    HtmlToken *t;
    while ((t = htmlParser->Next()) && !t->IsError()) {
        if (t->IsTag())
            HandleHtmlTag(t, doc);
        else
            EmitText(t);
    }
    // force layout of the last line
    StartNewLine(true);
    // only send the last page if not empty
    if (currPage && currPage->Count() > 0)
        StartNewPage();
}

void LayoutHtml(LayoutInfo* li, FontCache *fontCache, INewPageObserver *pageObserver)
{
    PageLayout l(fontCache);
    l.StartLayout(li);
    l.Process(pageObserver, li->doc);
}

void DrawPageLayout(Graphics *g, PageData *pageData, REAL offX, REAL offY, bool showBbox)
{
    InitGraphicsMode(g);

    SolidBrush br(Color(0,0,0));
    Pen redPen(Color(255, 0, 0), 1);
    Pen blackPen(Color(0, 0, 0), 1);

    Font *font = NULL;

    Vec<DrawInstr> *instrs = &pageData->drawInstructions;
    for (DrawInstr *instr = instrs->IterStart(); instr; instr = instrs->IterNext()) {
        RectF bbox = instr->bbox;
        bbox.X += offX;
        bbox.Y += offY;
        if (InstrTypeLine == instr->type) {
            // hr is a line drawn in the middle of bounding box
            REAL y = bbox.Y + bbox.Height / 2.f;
            PointF p1(bbox.X, y);
            PointF p2(bbox.X + bbox.Width, y);
            if (showBbox)
                g->DrawRectangle(&redPen, bbox);
            g->DrawLine(&blackPen, p1, p2);
        } else if (InstrTypeString == instr->type) {
            WCHAR buf[512];
            size_t strLen = str::Utf8ToWcharBuf(instr->str.s, instr->str.len, buf, dimof(buf));
            PointF pos;
            bbox.GetLocation(&pos);
            if (showBbox)
                g->DrawRectangle(&redPen, bbox);
            g->DrawString(buf, strLen, font, pos, NULL, &br);
        } else if (InstrTypeSetFont == instr->type) {
            font = instr->setFont.font;
        } else if (InstrTypeImage == instr->type) {
            Bitmap *bmp = BitmapFromData(instr->img.data->data, instr->img.data->len);
            if (bmp)
                g->DrawImage(bmp, bbox, 0, 0, (REAL)bmp->GetWidth(), (REAL)bmp->GetHeight(), UnitPixel);
            delete bmp;
        }
    }
}

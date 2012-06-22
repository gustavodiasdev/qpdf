#include <qpdf/QPDF.hh>

#include <assert.h>

#include <qpdf/QTC.hh>
#include <qpdf/QUtil.hh>
#include <qpdf/QPDFExc.hh>

// In support of page manipulation APIs, these methods internally
// maintain state about pages in a pair of data structures: all_pages,
// which is a vector of page objects, and pageobj_to_pages_pos, which
// maps a page object to its position in the all_pages array.
// Unfortunately, the getAllPages() method returns a const reference
// to all_pages and has been in the public API long before the
// introduction of mutation APIs, so we're pretty much stuck with it.
// Anyway, there are lots of calls to it in the library, so the
// efficiency of having it cached is probably worth keeping it.

// The goal of this code is to ensure that the all_pages vector, which
// users may have a reference to, and the pageobj_to_pages_pos map,
// which users will not have access to, remain consistent outside of
// any call to the library.  As long as users only touch the /Pages
// structure through page-specific API calls, they never have to worry
// about anything, and this will also stay consistent.  If a user
// touches anything about the /Pages structure outside of these calls
// (such as by directly looking up and manipulating the underlying
// objects), they can call updatePagesCache() to bring things back in
// sync.

// If the user doesn't ever use the page manipulation APIs, then qpdf
// leaves the /Pages structure alone.  If the user does use the APIs,
// then we push all inheritable objects down and flatten the /Pages
// tree.  This makes it easier for us to keep /Pages, all_pages, and
// pageobj_to_pages_pos internally consistent at all times.

// Responsibility for keeping all_pages, pageobj_to_pages_pos, and the
// Pages structure consistent should remain in as few places as
// possible.  As of initial writing, only flattenPagesTree,
// insertPage, and removePage, along with methods they call, are
// concerned with it.  Everything else goes through one of those
// methods.

std::vector<QPDFObjectHandle> const&
QPDF::getAllPages()
{
    if (this->all_pages.empty())
    {
	getAllPagesInternal(getRoot().getKey("/Pages"), this->all_pages);
    }
    return this->all_pages;
}

void
QPDF::getAllPagesInternal(QPDFObjectHandle cur_pages,
			  std::vector<QPDFObjectHandle>& result)
{
    std::string type = cur_pages.getKey("/Type").getName();
    if (type == "/Pages")
    {
	QPDFObjectHandle kids = cur_pages.getKey("/Kids");
	int n = kids.getArrayNItems();
	for (int i = 0; i < n; ++i)
	{
	    getAllPagesInternal(kids.getArrayItem(i), result);
	}
    }
    else if (type == "/Page")
    {
	result.push_back(cur_pages);
    }
    else
    {
	throw QPDFExc(qpdf_e_damaged_pdf, this->file->getName(),
		      this->last_object_description,
		      this->file->getLastOffset(),
		      "invalid Type " + type + " in page tree");
    }
}

void
QPDF::updateAllPagesCache()
{
    // Force regeneration of the pages cache.  We force immediate
    // recalculation of all_pages since users may have references to
    // it that they got from calls to getAllPages().  We can defer
    // recalculation of pageobj_to_pages_pos until needed.
    QTC::TC("qpdf", "QPDF updateAllPagesCache");
    this->all_pages.clear();
    this->pageobj_to_pages_pos.clear();
    getAllPages();
}

void
QPDF::flattenPagesTree()
{
    // If not already done, flatten the /Pages structure and
    // initialize pageobj_to_pages_pos.

    if (! this->pageobj_to_pages_pos.empty())
    {
        return;
    }

    // Push inherited objects down to the /Page level
    pushInheritedAttributesToPage(true, true);
    getAllPages();

    QPDFObjectHandle pages = getRoot().getKey("/Pages");

    int const len = (int)this->all_pages.size();
    for (int pos = 0; pos < len; ++pos)
    {
        // populate pageobj_to_pages_pos and fix parent pointer
        insertPageobjToPage(this->all_pages[pos], pos, true);
        this->all_pages[pos].replaceKey("/Parent", pages);
    }

    pages.replaceKey("/Kids", QPDFObjectHandle::newArray(this->all_pages));
    // /Count has not changed
    assert(pages.getKey("/Count").getIntValue() == len);
}

void
QPDF::insertPageobjToPage(QPDFObjectHandle const& obj, int pos,
                          bool check_duplicate)
{
    ObjGen og(obj.getObjectID(), obj.getGeneration());
    if (check_duplicate)
    {
        if (! this->pageobj_to_pages_pos.insert(std::make_pair(og, pos)).second)
        {
            QTC::TC("qpdf", "QPDF duplicate page reference");
            setLastObjectDescription("page " + QUtil::int_to_string(pos) +
                                     " (numbered from zero)",
                                     og.obj, og.gen);
            throw QPDFExc(qpdf_e_pages, this->file->getName(),
                          this->last_object_description, 0,
                          "duplicate page reference found;"
                          " this would cause loss of data");
        }
    }
    else
    {
        this->pageobj_to_pages_pos[og] = pos;
    }
}

void
QPDF::insertPage(QPDFObjectHandle newpage, int pos)
{
    // pos is numbered from 0, so pos = 0 inserts at the begining and
    // pos = npages adds to the end.

    flattenPagesTree();
    newpage.assertPageObject();

    if (! newpage.isIndirect())
    {
        QTC::TC("qpdf", "QPDF insert non-indirect page");
        newpage = this->makeIndirectObject(newpage);
    }
    else
    {
        QTC::TC("qpdf", "QPDF insert indirect page");
    }

    QTC::TC("qpdf", "QPDF insert page",
            (pos == 0) ? 0 :                      // insert at beginning
            (pos == ((int)this->all_pages.size())) ? 1 : // insert at end
            2);                                   // insert in middle

    QPDFObjectHandle pages = getRoot().getKey("/Pages");
    QPDFObjectHandle kids = pages.getKey("/Kids");
    assert ((pos >= 0) && (pos <= (int)this->all_pages.size()));

    newpage.replaceKey("/Parent", pages);
    kids.insertItem(pos, newpage);
    int npages = kids.getArrayNItems();
    pages.replaceKey("/Count", QPDFObjectHandle::newInteger(npages));
    this->all_pages.insert(this->all_pages.begin() + pos, newpage);
    assert((int)this->all_pages.size() == npages);
    for (int i = pos + 1; i < npages; ++i)
    {
        insertPageobjToPage(this->all_pages[i], i, false);
    }
    insertPageobjToPage(newpage, pos, true);
    assert((int)this->pageobj_to_pages_pos.size() == npages);
}

void
QPDF::removePage(QPDFObjectHandle page)
{
    int pos = findPage(page); // also ensures flat /Pages
    QTC::TC("qpdf", "QPDF remove page",
            (pos == 0) ? 0 :                          // remove at beginning
            (pos == ((int)this->all_pages.size() - 1)) ? 1 : // remove at end
            2);                                       // remove in middle

    QPDFObjectHandle pages = getRoot().getKey("/Pages");
    QPDFObjectHandle kids = pages.getKey("/Kids");

    kids.eraseItem(pos);
    int npages = kids.getArrayNItems();
    pages.replaceKey("/Count", QPDFObjectHandle::newInteger(npages));
    this->all_pages.erase(this->all_pages.begin() + pos);
    assert((int)this->all_pages.size() == npages);
    this->pageobj_to_pages_pos.erase(
        ObjGen(page.getObjectID(), page.getGeneration()));
    assert((int)this->pageobj_to_pages_pos.size() == npages);
    for (int i = pos; i < npages; ++i)
    {
        insertPageobjToPage(this->all_pages[i], i, false);
    }
}

void
QPDF::addPageAt(QPDFObjectHandle newpage, bool before,
                QPDFObjectHandle refpage)
{
    int refpos = findPage(refpage);
    if (! before)
    {
        ++refpos;
    }
    insertPage(newpage, refpos);
}

void
QPDF::addPage(QPDFObjectHandle newpage, bool first)
{
    getAllPages();
    if (first)
    {
        insertPage(newpage, 0);
    }
    else
    {
        insertPage(newpage, (int)this->all_pages.size());
    }
}

int
QPDF::findPage(QPDFObjectHandle& page)
{
    page.assertPageObject();
    return findPage(page.getObjectID(), page.getGeneration());
}

int
QPDF::findPage(int objid, int generation)
{
    flattenPagesTree();
    std::map<ObjGen, int>::iterator it =
        this->pageobj_to_pages_pos.find(ObjGen(objid, generation));
    if (it == this->pageobj_to_pages_pos.end())
    {
        setLastObjectDescription("page object", objid, generation);
        throw QPDFExc(qpdf_e_pages, this->file->getName(),
                      this->last_object_description, 0,
                      "page object not referenced in /Pages tree");
    }
    return (*it).second;
}

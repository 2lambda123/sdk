/**
 * @file node.cpp
 * @brief Classes for accessing local and remote nodes
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/node.h"
#include "mega/megaclient.h"
#include "mega/megaapp.h"
#include "mega/share.h"
#include "mega/serialize64.h"
#include "mega/base64.h"
#include "mega/sync.h"
#include "mega/transfer.h"
#include "mega/transferslot.h"
#include "mega/logging.h"
#include "mega/heartbeats.h"
#include "megafs.h"

namespace mega {

Node::Node(MegaClient& cclient, NodeHandle h, NodeHandle ph,
           nodetype_t t, m_off_t s, handle u, const char* fa, m_time_t ts)
    : client(&cclient)
{
    outshares = NULL;
    pendingshares = NULL;
    appdata = NULL;

    nodehandle = h.as8byte();
    parenthandle = ph.as8byte();

    parent = NULL;

    type = t;

    size = s;
    owner = u;

    JSON::copystring(&fileattrstring, fa);

    ctime = ts;

    inshare = NULL;
    sharekey = NULL;
    foreignkey = false;

    plink = NULL;

    memset(&changed, 0, sizeof changed);

    mFingerPrintPosition = client->mNodeManager.invalidFingerprintPos();

    if (type == FILENODE)
    {
        mCounter.files = 1;
        mCounter.storage = size;
    }
    else if (type == FOLDERNODE)
    {
        mCounter.folders = 1;
    }
}

Node::~Node()
{
    if (keyApplied())
    {
        client->mAppliedKeyNodeCount--;
        assert(client->mAppliedKeyNodeCount >= 0);
    }

    // abort pending direct reads
    client->preadabort(this);

    if (outshares)
    {
        // delete outshares, including pointers from users for this node
        for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
        {
            delete it->second;
        }
        delete outshares;
    }

    if (pendingshares)
    {
        // delete pending shares
        for (share_map::iterator it = pendingshares->begin(); it != pendingshares->end(); it++)
        {
            delete it->second;
        }
        delete pendingshares;
    }

    delete plink;
    delete inshare;
    delete sharekey;
}
int Node::getShareType() const
{
    int shareType = ShareType_t::NO_SHARES;

    if (inshare)
    {
        shareType |= ShareType_t::IN_SHARES;
    }
    else
    {
        if (outshares)
        {
            for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
            {
                Share *share = it->second;
                if (share->user)    // folder links are shares without user
                {
                    shareType |= ShareType_t::OUT_SHARES;
                    break;
                }
            }
        }
        if (pendingshares && pendingshares->size())
        {
            shareType |= ShareType_t::PENDING_OUTSHARES;
        }
        if (plink)
        {
            shareType |= ShareType_t::LINK;
        }
    }

    return shareType;
}

bool Node::isAncestor(NodeHandle ancestorHandle) const
{
    Node* ancestor = parent;
    while (ancestor)
    {
        if (ancestor->nodeHandle() == ancestorHandle)
        {
            return true;
        }

        ancestor = ancestor->parent;
    }

    return false;
}


bool Node::hasChildWithName(const string& name) const
{
    return client->childnodebyname(this, name.c_str()) ? true : false;
}

bool Node::getExtension(std::string& ext) const
{
    ext.clear();
    const char* name = displayname();
    const size_t size = strlen(name);

    const char* ptr = name + size;
    char c;

    for (unsigned i = 0; i < size; ++i)
    {
        if (*--ptr == '.')
        {
            ptr++; // Avoid add dot
            ext.reserve(i);

            unsigned j = 0;
            for (; j <= i - 1; j++)
            {
                if (*ptr < '.' || *ptr > 'z') return false;

                c = *(ptr++);

                // tolower()
                if (c >= 'A' && c <= 'Z') c |= ' ';

                ext.push_back(c);
            }

            return true;
        }
    }

    return false;
}

// these lists of file extensions (and the logic to use them) all come from the webclient - if updating here, please make sure the webclient is updated too, preferably webclient first.

static const std::set<nameid> documentExtensions = {MAKENAMEID3('a','n','s'), MAKENAMEID5('a','s','c','i','i'), MAKENAMEID3('d','o','c'), MAKENAMEID4('d','o','c','x'), MAKENAMEID4('d','o','t', 'x'), MAKENAMEID4('j','s','o','n'),  MAKENAMEID3('l','o','g'), MAKENAMEID3('o','d','s'), MAKENAMEID3('o','d','t'), MAKENAMEID5('p','a','g','e','s'), MAKENAMEID3('p','d','f'), MAKENAMEID3('p','p','c'), MAKENAMEID3('p','p','s'), MAKENAMEID3('p','p','t'), MAKENAMEID4('p','p','t','x'), MAKENAMEID3('r','t','f'),
                                             MAKENAMEID3('s','t','c'), MAKENAMEID3('s','t','d'), MAKENAMEID3('s','t','w'), MAKENAMEID3('s','t','i'), MAKENAMEID3('s','x','c'), MAKENAMEID3('s','x','d'), MAKENAMEID3('s','x','i'), MAKENAMEID3('s','x','m'), MAKENAMEID3('s','x','w'), MAKENAMEID3('t','x','t'), MAKENAMEID3('w','p','d'), MAKENAMEID3('w','p','s'), MAKENAMEID3('x','l','s'), MAKENAMEID4('x','l','s','x'), MAKENAMEID3('x','l','t'), MAKENAMEID4('x','l','t','m')};

static const std::set<nameid> audioExtensions = {MAKENAMEID3('a','c','3'), MAKENAMEID3('e','c','3'), MAKENAMEID3('3','g','a'), MAKENAMEID3('a','a','c'), MAKENAMEID3('a','d','p'), MAKENAMEID3('a','i','f'), MAKENAMEID4('a','i','f','c'), MAKENAMEID4('a','i','f','f'), MAKENAMEID2('a','u'), MAKENAMEID3('c','a','f'), MAKENAMEID3('d','r','a'), MAKENAMEID3('d','t','s'), MAKENAMEID5('d','t','s','h','d'), MAKENAMEID3('e','o','l'), MAKENAMEID4('f','l','a','c'), MAKENAMEID3('i','f','f'), MAKENAMEID3('k','a','r'), MAKENAMEID3('l','v','p'),
                                          MAKENAMEID3('m','2','a'), MAKENAMEID3('m','3','a'), MAKENAMEID3('m','3','u'), MAKENAMEID3('m','4','a'), MAKENAMEID3('m','i','d'), MAKENAMEID4('m','i','d','i'), MAKENAMEID3('m','k','a'), MAKENAMEID3('m','p','2'), MAKENAMEID4('m','p','2','a'), MAKENAMEID3('m','p','3'), MAKENAMEID4('m','p','4','a'), MAKENAMEID4('m','p','g','a'), MAKENAMEID3('o','g','a'), MAKENAMEID3('o','g','g'), MAKENAMEID4('o','p','u','s'), MAKENAMEID3('p','y','a'), MAKENAMEID2('r','a'),
                                          MAKENAMEID3('r','a','m'), MAKENAMEID3('r','i','p'), MAKENAMEID3('r','m','i'), MAKENAMEID3('r','m','p'), MAKENAMEID3('s','3','m'), MAKENAMEID3('s','i','l'), MAKENAMEID3('s','n','d'), MAKENAMEID3('s','p','x'), MAKENAMEID3('u','v','a'), MAKENAMEID4('u','v','v','a'), MAKENAMEID3('w','a','v'), MAKENAMEID3('w','a','x'), MAKENAMEID4('w','e','b','a'), MAKENAMEID3('w','m','a'), MAKENAMEID2('x','m')};

// Store extension than can't be stored in nameid due they have more than 8 characters
static const std::set<std::string> longAudioExtension = {"ecelp4800", "ecelp7470", "ecelp9600"};

static const std::set<nameid> videoExtensions = {MAKENAMEID3('3','g','2'), MAKENAMEID3('3','g','p'), MAKENAMEID3('a','s','f'), MAKENAMEID3('a','s','x'), MAKENAMEID3('a','v','i'), MAKENAMEID3('d','v','b'), MAKENAMEID3('f','4','v'), MAKENAMEID3('f','l','i'), MAKENAMEID3('f','l','v'), MAKENAMEID3('f','v','t'), MAKENAMEID4('h','2','6','1'), MAKENAMEID4('h','2','6','3'), MAKENAMEID4('h','2','6','4'), MAKENAMEID4('j','p','g','m'), MAKENAMEID4('j','p','g','v'), MAKENAMEID3('j','p','m'), MAKENAMEID3('m','1','v'),
                                          MAKENAMEID3('m','2','v'), MAKENAMEID3('m','4','u'), MAKENAMEID3('m','4','v'), MAKENAMEID3('m','j','2'), MAKENAMEID4('m','j','p','2'), MAKENAMEID4('m','k','3','d'), MAKENAMEID3('m','k','s'), MAKENAMEID3('m','k','v'), MAKENAMEID3('m','n','g'), MAKENAMEID3('m','o','v'), MAKENAMEID5('m','o','v','i','e'), MAKENAMEID3('m','p','4'), MAKENAMEID4('m','p','4','v'), MAKENAMEID3('m','p','e'), MAKENAMEID4('m','p','e','g'), MAKENAMEID3('m','p','g'), MAKENAMEID4('m','p','g','4'),
                                          MAKENAMEID3('m','x','u'), MAKENAMEID3('o','g','v'), MAKENAMEID3('p','y','v'), MAKENAMEID2('q','t'), MAKENAMEID3('s','m','v'), MAKENAMEID3('u','v','h'), MAKENAMEID3('u','v','m'), MAKENAMEID3('u','v','p'), MAKENAMEID3('u','v','s'), MAKENAMEID3('u','v','u'), MAKENAMEID3('u','v','v'), MAKENAMEID4('u','v','v','h'), MAKENAMEID4('u','v','v','m'), MAKENAMEID4('u','v','v','p'), MAKENAMEID4('u','v','v','s'), MAKENAMEID4('u','v','v','u'), MAKENAMEID4('u','v','v','v'),
                                          MAKENAMEID3('v','i','v'), MAKENAMEID3('v','o','b'), MAKENAMEID4('w','e','b','m'), MAKENAMEID2('w','m'), MAKENAMEID3('w','m','v'), MAKENAMEID3('w','m','x'), MAKENAMEID3('w','v','x')};

static const std::set<nameid> photoExtensions = {MAKENAMEID3('3','d','s'), MAKENAMEID3('b','m','p'), MAKENAMEID4('b','t','i','f'), MAKENAMEID3('c','g','m'), MAKENAMEID3('c','m','x'), MAKENAMEID3('d','j','v'), MAKENAMEID4('d','j','v','u'), MAKENAMEID3('d','w','g'), MAKENAMEID3('d','x','f'), MAKENAMEID3('f','b','s'), MAKENAMEID2('f','h'), MAKENAMEID3('f','h','4'), MAKENAMEID3('f','h','5'), MAKENAMEID3('f','h','7'), MAKENAMEID3('f','h','c'), MAKENAMEID3('f','p','x'), MAKENAMEID3('f','s','t'), MAKENAMEID2('g','3'),
                                          MAKENAMEID3('g','i','f'), MAKENAMEID4('h','e','i','c'), MAKENAMEID4('h','e','i','f'), MAKENAMEID3('i','c','o'), MAKENAMEID3('i','e','f'), MAKENAMEID3('j','p','e'), MAKENAMEID4('j','p','e','g'), MAKENAMEID3('j','p','g'), MAKENAMEID3('k','t','x'), MAKENAMEID3('m','d','i'), MAKENAMEID3('m','m','r'), MAKENAMEID3('n','p','x'), MAKENAMEID3('p','b','m'), MAKENAMEID3('p','c','t'), MAKENAMEID3('p','c','x'), MAKENAMEID3('p','g','m'), MAKENAMEID3('p','i','c'),
                                          MAKENAMEID3('p','n','g'), MAKENAMEID3('p','n','m'), MAKENAMEID3('p','p','m'), MAKENAMEID3('p','s','d'), MAKENAMEID3('r','a','s'), MAKENAMEID3('r','g','b'), MAKENAMEID3('r','l','c'), MAKENAMEID3('s','g','i'), MAKENAMEID3('s','i','d'), MAKENAMEID3('s','v','g'), MAKENAMEID4('s','v','g','z'), MAKENAMEID3('t','g','a'), MAKENAMEID3('t','i','f'), MAKENAMEID4('t','i','f','f'), MAKENAMEID3('u','v','g'), MAKENAMEID3('u','v','i'), MAKENAMEID4('u','v','v','g'),
                                          MAKENAMEID4('u','v','v','i'), MAKENAMEID4('w','b','m','p'), MAKENAMEID3('w','d','p'), MAKENAMEID4('w','e','b','p'), MAKENAMEID3('x','b','m'), MAKENAMEID3('x','i','f'), MAKENAMEID3('x','p','m'), MAKENAMEID3('x','w','d')};

static const std::set<nameid> photoRawExtensions = {MAKENAMEID3('3','f','r'), MAKENAMEID3('a','r','w'), MAKENAMEID3('c','r','2'), MAKENAMEID3('c','r','w'), MAKENAMEID4('c','i','f','f'), MAKENAMEID3('c','s','1'), MAKENAMEID3('d','c','r'), MAKENAMEID3('d','n','g'), MAKENAMEID3('e','r','f'), MAKENAMEID3('i','i','q'), MAKENAMEID3('k','2','5'), MAKENAMEID3('k','d','c'), MAKENAMEID3('m','e','f'), MAKENAMEID3('m','o','s'), MAKENAMEID3('m','r','w'), MAKENAMEID3('n','e','f'), MAKENAMEID3('n','r','w'),
                                          MAKENAMEID3('o','r','f'), MAKENAMEID3('p','e','f'), MAKENAMEID3('r','a','f'), MAKENAMEID3('r','a','w'), MAKENAMEID3('r','w','2'), MAKENAMEID3('r','w','l'), MAKENAMEID3('s','r','2'), MAKENAMEID3('s','r','f'), MAKENAMEID3('s','r','w'), MAKENAMEID3('x','3','f')};

static const std::set<nameid> photoImageDefExtension = {MAKENAMEID3('j','p','g'), MAKENAMEID4('j','p','e','g'), MAKENAMEID3('g','i','f'), MAKENAMEID3('b','m','p'), MAKENAMEID3('p','n','g')};

bool Node::isPhoto(const std::string& ext, bool checkPreview) const
{
    nameid extNameid = getExtensionNameId(ext);
    // evaluate according to the webclient rules, so that we get exactly the same bucketing.
    return photoImageDefExtension.find(extNameid) != photoImageDefExtension.end() ||
        photoRawExtensions.find(extNameid) != photoRawExtensions.end() ||
        (photoExtensions.find(extNameid) != photoExtensions.end()
            && (!checkPreview || hasfileattribute(GfxProc::PREVIEW)));
}

bool Node::isVideo(const std::string& ext) const
{
    if (hasfileattribute(fa_media) && nodekey().size() == FILENODEKEYLENGTH)
    {
#ifdef USE_MEDIAINFO
        if (client->mediaFileInfo.mediaCodecsReceived)
        {
            MediaProperties mp = MediaProperties::decodeMediaPropertiesAttributes(fileattrstring, (uint32_t*)(nodekey().data() + FILENODEKEYLENGTH / 2));
            unsigned videocodec = mp.videocodecid;
            if (!videocodec && mp.shortformat)
            {
                auto& v = client->mediaFileInfo.mediaCodecs.shortformats;
                if (mp.shortformat < v.size())
                {
                    videocodec = v[mp.shortformat].videocodecid;
                }
            }
            // approximation: the webclient has a lot of logic to determine if a particular codec is playable in that browser.  We'll just base our decision on the presence of a video codec.
            if (!videocodec)
            {
                return false; // otherwise double-check by extension
            }
        }
#endif
    }

    return videoExtensions.find(getExtensionNameId(ext)) != videoExtensions.end();
}

bool Node::isAudio(const std::string& ext) const
{
    nameid extNameid = getExtensionNameId(ext);
    if (extNameid != 0)
    {
        return audioExtensions.find(extNameid) != audioExtensions.end();
    }

    // Check longer extension
    return longAudioExtension.find(ext) != longAudioExtension.end();
}

bool Node::isDocument(const std::string& ext) const
{
    return documentExtensions.find(getExtensionNameId(ext)) != documentExtensions.end();
}

nameid Node::getExtensionNameId(const std::string& ext)
{
    if (ext.length() > 8)
    {
        return 0;
    }

    JSON json;
    return json.getnameid(ext.c_str());
}

void Node::setkeyfromjson(const char* k)
{
    if (keyApplied()) --client->mAppliedKeyNodeCount;
    JSON::copystring(&nodekeydata, k);
    if (keyApplied()) ++client->mAppliedKeyNodeCount;
    assert(client->mAppliedKeyNodeCount >= 0);
}

void Node::setUndecryptedKey(const std::string& undecryptedKey)
{
    nodekeydata = undecryptedKey;
}

// update node key and decrypt attributes
void Node::setkey(const byte* newkey)
{
    if (newkey)
    {
        if (keyApplied()) --client->mAppliedKeyNodeCount;
        nodekeydata.assign(reinterpret_cast<const char*>(newkey), (type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH);
        if (keyApplied()) ++client->mAppliedKeyNodeCount;
        assert(client->mAppliedKeyNodeCount >= 0);
    }

    setattr();
}

// serialize node - nodes with pending or RSA keys are unsupported
bool Node::serialize(string* d)
{
    // do not serialize encrypted nodes
    if (attrstring)
    {
        LOG_debug << "Trying to serialize an encrypted node";

        //Last attempt to decrypt the node
        applykey(true);
        setattr();

        if (attrstring)
        {
            LOG_debug << "Serializing an encrypted node.";
        }
    }

    switch (type)
    {
        case FILENODE:
            if (!attrstring && (int)nodekeydata.size() != FILENODEKEYLENGTH)
            {
                return false;
            }
            break;

        case FOLDERNODE:
            if (!attrstring && (int)nodekeydata.size() != FOLDERNODEKEYLENGTH)
            {
                return false;
            }
            break;

        default:
            if (nodekeydata.size())
            {
                return false;
            }
    }

    unsigned short ll;
    short numshares;
    m_off_t s;

    s = type ? -type : size;

    d->append((char*)&s, sizeof s);

    d->append((char*)&nodehandle, MegaClient::NODEHANDLE);

    if (parenthandle != UNDEF)
    {
        d->append((char*)&parenthandle, MegaClient::NODEHANDLE);
    }
    else
    {
        d->append("\0\0\0\0\0", MegaClient::NODEHANDLE);
    }

    d->append((char*)&owner, MegaClient::USERHANDLE);

    // FIXME: use Serialize64
    time_t ts = 0;  // we don't want to break backward compatibility by changing the size (where m_time_t differs)
    d->append((char*)&ts, sizeof(ts));

    ts = (time_t)ctime;
    d->append((char*)&ts, sizeof(ts));

    if (attrstring)
    {
        auto length = 0u;

        if (type == FOLDERNODE)
        {
            length = FOLDERNODEKEYLENGTH;
        }
        else if (type == FILENODE)
        {
            length = FILENODEKEYLENGTH;
        }

        d->append(length, '\0');
    }
    else
    {
        d->append(nodekeydata);
    }

    if (type == FILENODE)
    {
        ll = static_cast<unsigned short>(fileattrstring.size() + 1);
        d->append((char*)&ll, sizeof ll);
        d->append(fileattrstring.c_str(), ll);
    }

    char isExported = plink ? 1 : 0;
    d->append((char*)&isExported, 1);

    char hasLinkCreationTs = plink ? 1 : 0;
    d->append((char*)&hasLinkCreationTs, 1);

    if (isExported && plink && plink->mAuthKey.size())
    {
        auto authKeySize = (char)plink->mAuthKey.size();
        d->append((char*)&authKeySize, sizeof(authKeySize));
        d->append(plink->mAuthKey.data(), authKeySize);
    }
    else
    {
        d->append("", 1);
    }

    d->append(1, static_cast<char>(!!attrstring));

    if (attrstring)
    {
        d->append(1, '\1');
    }

    // Use these bytes for extensions.
    d->append(4, '\0');

    if (inshare)
    {
        numshares = -1;
    }
    else
    {
        numshares = 0;
        if (outshares)
        {
            numshares = static_cast<short int>(numshares + outshares->size());
        }
        if (pendingshares)
        {
            numshares = static_cast<short int>(numshares + pendingshares->size());
        }
    }

    d->append((char*)&numshares, sizeof numshares);

    if (numshares)
    {
        d->append((char*)sharekey->key, SymmCipher::KEYLENGTH);

        if (inshare)
        {
            inshare->serialize(d);
        }
        else
        {
            if (outshares)
            {
                for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
                {
                    it->second->serialize(d);
                }
            }
            if (pendingshares)
            {
                for (share_map::iterator it = pendingshares->begin(); it != pendingshares->end(); it++)
                {
                    it->second->serialize(d);
                }
            }
        }
    }

    // Encrypted nodes have no attributes.
    if (attrstring)
    {
        d->append(1, '\0');
    }
    else
    {
        attrs.serialize(d);
    }

    if (isExported)
    {
        d->append((char*) &plink->ph, MegaClient::NODEHANDLE);
        d->append((char*) &plink->ets, sizeof(plink->ets));
        d->append((char*) &plink->takendown, sizeof(plink->takendown));
        if (hasLinkCreationTs)
        {
            d->append((char*) &plink->cts, sizeof(plink->cts));
        }
    }

    // Write data necessary to thaw encrypted nodes.
    if (attrstring)
    {
        // Write node key data.  These can be quite long, and can be in many shares.  Use 64 bit len
        uint64_t length1 = nodekeydata.size();
        d->append((char*)&length1, sizeof(length1));
        d->append(nodekeydata, 0, size_t(length1));

        // Write attribute string data.   Attributes can be long, too
        uint64_t length2 = attrstring->size();
        d->append((char*)&length2, sizeof(length2));
        d->append(*attrstring, 0, size_t(length2));
    }

    return true;
}

// decrypt attrstring and check magic number prefix
byte* Node::decryptattr(SymmCipher* key, const char* attrstring, size_t attrstrlen)
{
    if (attrstrlen)
    {
        int l = int(attrstrlen * 3 / 4 + 3);
        byte* buf = new byte[l];

        l = Base64::atob(attrstring, buf, l);

        if (!(l & (SymmCipher::BLOCKSIZE - 1)))
        {
            key->cbc_decrypt(buf, l);

            if (!memcmp(buf, "MEGA{\"", 6))
            {
                return buf;
            }
        }

        delete[] buf;
    }

    return NULL;
}

void Node::parseattr(byte *bufattr, AttrMap &attrs, m_off_t size, m_time_t &mtime , string &fileName, string &fingerprint, FileFingerprint &ffp)
{
    JSON json;
    nameid name;
    string *t;

    json.begin((char*)bufattr + 5);
    while ((name = json.getnameid()) != EOO && json.storeobject((t = &attrs.map[name])))
    {
        JSON::unescape(t);
    }

    attr_map::iterator it = attrs.map.find('n');   // filename
    if (it == attrs.map.end())
    {
        fileName = "CRYPTO_ERROR";
    }
    else if (it->second.empty())
    {
        fileName = "BLANK";
    }

    it = attrs.map.find('c');   // checksum
    if (it != attrs.map.end())
    {
        if (ffp.unserializefingerprint(&it->second))
        {
            ffp.size = size;
            mtime = ffp.mtime;

            char bsize[sizeof(size) + 1];
            int l = Serialize64::serialize((byte *)bsize, size);
            char *buf = new char[l * 4 / 3 + 4];
            char ssize = static_cast<char>('A' + Base64::btoa((const byte *)bsize, l, buf));

            string result(1, ssize);
            result.append(buf);
            result.append(it->second);
            delete [] buf;

            fingerprint = result;
        }
    }
}

// return temporary SymmCipher for this nodekey
SymmCipher* Node::nodecipher()
{
    return client->getRecycledTemporaryNodeCipher(&nodekeydata);
}

// decrypt attributes and build attribute hash
void Node::setattr()
{
    byte* buf;
    SymmCipher* cipher;

    if (attrstring && (cipher = nodecipher()) && (buf = decryptattr(cipher, attrstring->c_str(), attrstring->size())))
    {
        JSON json;
        nameid name;
        string* t;

        AttrMap oldAttrs(attrs);
        attrs.map.clear();
        json.begin((char*)buf + 5);

        while ((name = json.getnameid()) != EOO && json.storeobject((t = &attrs.map[name])))
        {
            JSON::unescape(t);

            if (name == 'n')
            {
                LocalPath::utf8_normalize(t);
            }
        }

        changed.name = attrs.hasDifferentValue('n', oldAttrs.map);
        changed.favourite = attrs.hasDifferentValue(AttrMap::string2nameid("fav"), oldAttrs.map);

        setfingerprint();

        delete[] buf;

        attrstring.reset();
    }
}

nameid Node::sdsId()
{
    constexpr nameid nid = MAKENAMEID3('s', 'd', 's');
    return nid;
}

vector<pair<handle, int>> Node::getSdsBackups() const
{
    vector<pair<handle, int>> bkps;

    auto it = attrs.map.find(sdsId());
    if (it != attrs.map.end())
    {
        std::istringstream is(it->second);  // "b64aa:8,b64bb:8"
        while (!is.eof())
        {
            string b64BkpIdStr;
            std::getline(is, b64BkpIdStr, ':');
            if (!is.good())
            {
                LOG_err << "Invalid format in 'sds' attr value for backup id";
                break;
            }
            handle bkpId = UNDEF;
            Base64::atob(b64BkpIdStr.c_str(), (byte*)&bkpId, MegaClient::BACKUPHANDLE);
            assert(bkpId != UNDEF);

            string stateStr;
            std::getline(is, stateStr, ',');
            try
            {
                int state = std::stoi(stateStr);
                bkps.push_back(std::make_pair(bkpId, state));
            }
            catch (...)
            {
                LOG_err << "Invalid backup state in 'sds' attr value";
                break;
            }
        }
    }

    return bkps;
}

string Node::toSdsString(const vector<pair<handle, int>>& ids)
{
    string value;

    for (const auto& i : ids)
    {
        std::string idStr(Base64Str<MegaClient::BACKUPHANDLE>(i.first));
        value += idStr + ':' + std::to_string(i.second) + ','; // `b64aa:8,b64bb:8,`
    }

    if (!value.empty())
    {
        value.pop_back(); // remove trailing ','
    }

    return value;
}

// if present, configure FileFingerprint from attributes
// otherwise, the file's fingerprint is derived from the file's mtime/size/key
void Node::setfingerprint()
{
    if (type == FILENODE && nodekeydata.size() >= sizeof crc)
    {
        client->mNodeManager.removeFingerprint(this);

        attr_map::iterator it = attrs.map.find('c');

        if (it != attrs.map.end())
        {
            if (!unserializefingerprint(&it->second))
            {
                LOG_warn << "Invalid fingerprint";
            }
        }

        // if we lack a valid FileFingerprint for this file, use file's key,
        // size and client timestamp instead
        if (!isvalid)
        {
            memcpy(crc.data(), nodekeydata.data(), sizeof crc);
            mtime = ctime;
        }

        mFingerPrintPosition = client->mNodeManager.insertFingerprint(this);
    }
}

bool Node::hasName(const string& name) const
{
    auto it = attrs.map.find('n');
    return it != attrs.map.end() && it->second == name;
}

bool Node::hasName() const
{
    auto i = attrs.map.find('n');

    return i != attrs.map.end() && !i->second.empty();
}

// return file/folder name or special status strings
const char* Node::displayname() const
{
    // not yet decrypted
    if (attrstring)
    {
        LOG_debug << "NO_KEY " << type << " " << size << " " << Base64Str<MegaClient::NODEHANDLE>(nodehandle);
        return "NO_KEY";
    }

    attr_map::const_iterator it;

    it = attrs.map.find('n');

    if (it == attrs.map.end())
    {
        if (type < ROOTNODE || type > RUBBISHNODE)
        {
            LOG_debug << "CRYPTO_ERROR " << type << " " << size << " " << nodehandle;
        }
        return "CRYPTO_ERROR";
    }

    if (!it->second.size())
    {
        LOG_debug << "BLANK " << type << " " << size << " " << nodehandle;
        return "BLANK";
    }

    return it->second.c_str();
}

string Node::displaypath() const
{
    // factored from nearly identical functions in megapi_impl and megacli
    string path;
    const Node* n = this;
    for (; n; n = n->parent)
    {
        switch (n->type)
        {
        case FOLDERNODE:
            path.insert(0, n->displayname());

            if (n->inshare)
            {
                path.insert(0, ":");
                if (n->inshare->user)
                {
                    path.insert(0, n->inshare->user->email);
                }
                else
                {
                    path.insert(0, "UNKNOWN");
                }
                return path;
            }
            break;

        case VAULTNODE:
            path.insert(0, "//in");
            return path;

        case ROOTNODE:
            return path.empty() ? "/" : path;

        case RUBBISHNODE:
            path.insert(0, "//bin");
            return path;

        case TYPE_DONOTSYNC:
        case TYPE_SPECIAL:
        case TYPE_UNKNOWN:
        case FILENODE:
            path.insert(0, n->displayname());
        }
        path.insert(0, "/");
    }
    return path;
}

MimeType_t Node::getMimeType(bool checkPreview) const
{
    if (type != FILENODE)
    {
        return MimeType_t::MIME_TYPE_UNKNOWN;
    }

    std::string extension;
    if (!getExtension(extension))
    {
        return MimeType_t::MIME_TYPE_UNKNOWN;
    }

    if (isPhoto(extension, checkPreview))
    {
        return MimeType_t::MIME_TYPE_PHOTO;
    }
    else if (isVideo(extension))
    {
        return MimeType_t::MIME_TYPE_VIDEO;
    }
    else if (isAudio(extension))
    {
        return MimeType_t::MIME_TYPE_AUDIO;
    }
    else if (isDocument(extension))
    {
        return MimeType_t::MIME_TYPE_DOCUMENT;
    }

    return MimeType_t::MIME_TYPE_UNKNOWN;
}

// returns position of file attribute or 0 if not present
int Node::hasfileattribute(fatype t) const
{
    return Node::hasfileattribute(&fileattrstring, t);
}

int Node::hasfileattribute(const string *fileattrstring, fatype t)
{
    char buf[24];

    sprintf(buf, ":%u*", t);
    return static_cast<int>(fileattrstring->find(buf) + 1);
}

// attempt to apply node key - sets nodekey to a raw key if successful
bool Node::applykey(bool notAppliedOk)
{
    if (type > FOLDERNODE)
    {
        //Root nodes contain an empty attrstring
        attrstring.reset();
    }

    if (keyApplied() || !nodekeydata.size())
    {
        return false;
    }

    int l = -1;
    size_t t = 0;
    handle h;
    const char* k = NULL;
    SymmCipher* sc = &client->key;
    handle me = client->loggedin() ? client->me : client->mNodeManager.getRootNodeFiles().as8byte();

    while ((t = nodekeydata.find_first_of(':', t)) != string::npos)
    {
        // compound key: locate suitable subkey (always symmetric)
        h = 0;

        l = Base64::atob(nodekeydata.c_str() + (nodekeydata.find_last_of('/', t) + 1), (byte*)&h, sizeof h);
        t++;

        if (l == MegaClient::USERHANDLE)
        {
            // this is a user handle - reject if it's not me
            if (h != me)
            {
                continue;
            }
        }
        else
        {
            // look for share key if not folder access with folder master key
            if (h != me)
            {
                // this is a share node handle - check if share key is available at key's repository
                // if not available, check if the node already has the share key
                auto it = client->mNewKeyRepository.find(NodeHandle().set6byte(h));
                if (it == client->mNewKeyRepository.end())
                {
                    Node* n;
                    if (!(n = client->nodebyhandle(h)) || !n->sharekey)
                    {
                        continue;
                    }

                    sc = n->sharekey;
                }
                else
                {
                    sc = it->second.get();
                }

                // this key will be rewritten when the node leaves the outbound share
                foreignkey = true;
            }
        }

        k = nodekeydata.c_str() + t;
        break;
    }

    // no: found => personal key, use directly
    // otherwise, no suitable key available yet - bail (it might arrive soon)
    if (!k)
    {
        if (l < 0)
        {
            k = nodekeydata.c_str();
        }
        else
        {
            return false;
        }
    }

    byte key[FILENODEKEYLENGTH];
    unsigned keylength = (type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH;

    if (client->decryptkey(k, key, keylength, sc, 0, nodehandle))
    {
        client->mAppliedKeyNodeCount++;
        nodekeydata.assign((const char*)key, keylength);
        setattr();
    }

    bool applied = keyApplied();
    if (!applied)
    {
        LOG_warn << "Failed to apply key for node: " << Base64Str<MegaClient::NODEHANDLE>(nodehandle);
        // keys could be missing due to nested inshares with multiple users: user A shares a folder 1
        // with user B and folder 1 has a subfolder folder 1_1. User A shares folder 1_1 with user C
        // and user C adds some files, which will be undecryptable for user B.
        // The ticket SDK-1959 aims to mitigate the problem. Uncomment next line when done:
        // assert(applied);
    }

    return applied;
}

NodeCounter Node::getCounter() const
{
    return mCounter;
}

void Node::setCounter(const NodeCounter &counter, bool notify)
{
    mCounter = counter;

    if (notify)
    {
        changed.counter = true;
        client->notifynode(this);
    }
}

// returns whether node was moved
bool Node::setparent(Node* p, bool updateNodeCounters)
{
    if (p == parent)
    {
        return false;
    }

    Node *oldparent = parent;
    if (oldparent)
    {
        client->mNodeManager.removeChild(oldparent, nodeHandle());
    }

    parenthandle = p ? p->nodehandle : UNDEF;
    parent = p;
    if (parent)
    {
        client->mNodeManager.addChild(parent->nodeHandle(), nodeHandle(), this);
    }

    if (updateNodeCounters)
    {
        client->mNodeManager.updateCounter(*this, oldparent);
    }

//#ifdef ENABLE_SYNC
//    client->cancelSyncgetsOutsideSync(this);
//#endif

    return true;
}

const Node* Node::firstancestor() const
{
    const Node* n = this;
    while (n->parent != NULL)
    {
        n = n->parent;
    }

    return n;
}

const Node* Node::latestFileVersion() const
{
    const Node* n = this;
    if (type == FILENODE)
    {
        while (n->parent && n->parent->type == FILENODE)
        {
            n = n->parent;
        }
    }
    return n;
}

unsigned Node::depth() const
{
    auto* node = latestFileVersion();
    unsigned depth = 0u;

    for ( ; node->parent; node = node->parent)
        ++depth;

    return depth;
}

// returns 1 if n is under p, 0 otherwise
bool Node::isbelow(Node* p) const
{
    const Node* n = this;

    for (;;)
    {
        if (!n)
        {
            return false;
        }

        if (n == p)
        {
            return true;
        }

        n = n->parent;
    }
}

bool Node::isbelow(NodeHandle p) const
{
    const Node* n = this;

    for (;;)
    {
        if (!n)
        {
            return false;
        }

        if (n->nodeHandle() == p)
        {
            return true;
        }

        n = n->parent;
    }
}

void Node::setpubliclink(handle ph, m_time_t cts, m_time_t ets, bool takendown, const string &authKey)
{
    if (!plink) // creation
    {
        plink = new PublicLink(ph, cts, ets, takendown, authKey.empty() ? nullptr : authKey.c_str());
    }
    else            // update
    {
        plink->ph = ph;
        plink->cts = cts;
        plink->ets = ets;
        plink->takendown = takendown;
        plink->mAuthKey = authKey;
    }
}

PublicLink::PublicLink(handle ph, m_time_t cts, m_time_t ets, bool takendown, const char *authKey)
{
    this->ph = ph;
    this->cts = cts;
    this->ets = ets;
    this->takendown = takendown;
    if (authKey)
    {
        this->mAuthKey = authKey;
    }
}

PublicLink::PublicLink(PublicLink *plink)
{
    this->ph = plink->ph;
    this->cts = plink->cts;
    this->ets = plink->ets;
    this->takendown = plink->takendown;
    this->mAuthKey = plink->mAuthKey;
}

bool PublicLink::isExpired()
{
    if (!ets)       // permanent link: ets=0
        return false;

    m_time_t t = m_time();
    return ets < t;
}

#ifdef ENABLE_SYNC
// set, change or remove LocalNode's parent and localname/slocalname.
// newlocalpath must be a leaf name and must not point to an empty string (unless newparent == NULL).
// no shortname allowed as the last path component.
void LocalNode::setnameparent(LocalNode* newparent, const LocalPath& newlocalpath, std::unique_ptr<LocalPath> newshortname)
{
    Sync* oldsync = NULL;

    if (newshortname && *newshortname == newlocalpath)
    {
        // if the short name is the same, don't bother storing it.
        newshortname.reset();
    }

    bool parentChange = newparent != parent;
    bool localnameChange = newlocalpath != localname;
    bool shortnameChange = (newshortname && !slocalname) ||
                           (slocalname && !newshortname) ||
                           (newshortname && slocalname && *newshortname != *slocalname);

    if (parent)
    {
        if (parentChange || localnameChange)
        {
            // remove existing child linkage for localname
            auto it = parent->children.find(localname);
            if (it != parent->children.end() && it->second == this)
            {
                parent->children.erase(it);
            }
        }

        if (slocalname && (
            parentChange || shortnameChange))
        {
            // remove existing child linkage for slocalname
            auto it = parent->schildren.find(*slocalname);
            if (it != parent->schildren.end() && it->second == this)
            {
                parent->schildren.erase(it);
            }
        }
    }

    // reset treestate for old subtree (before we update the names for this node, in case we generate paths while recursing)
    // in case of just not syncing that subtree anymore - updates icon overlays
    if (parent && !newparent && !sync->mDestructorRunning)
    {
        // since we can't do it after the parent is updated
        // send out notifications with the current (soon to be old) paths, saying these are not consdiered by the sync anymore
        recursiveSetAndReportTreestate(TREESTATE_NONE, true, true);
    }

    if (localnameChange)
    {
        // set new name
        localname = newlocalpath;
        toName_of_localname = localname.toName(*sync->syncs.fsaccess);
    }

    if (shortnameChange)
    {
        // set new shortname
        slocalname = move(newshortname);
    }


    if (parentChange)
    {
        parent = newparent;

        if (parent && sync != parent->sync)
        {
            oldsync = sync;
            LOG_debug << "Moving files between different syncs";
        }
    }

    // add to parent map by localname
    if (parent && (parentChange || localnameChange))
    {
        #ifdef DEBUG
            auto it = parent->children.find(localname);
            assert(it == parent->children.end());   // check we are not about to orphan the old one at this location... if we do then how did we get a clash in the first place?
        #endif

        parent->children[localname] = this;
    }

    // add to parent map by shortname
    if (parent && slocalname && (parentChange || shortnameChange))
    {
        // it's quite possible that the new folder still has an older LocalNode with clashing shortname, that represents a file/folder since moved, but which we don't know about yet.
        // just assign the new one, we forget the old reference.  The other LocalNode will not remove this one since the LocalNode* will not match.
        parent->schildren[*slocalname] = this;
    }

    // reset treestate
    if (parent && parentChange && !sync->mDestructorRunning)
    {
        // As we recurse through the update tree, we will see
        // that it's different from this, and send out the true state
        recursiveSetAndReportTreestate(TREESTATE_NONE, true, false);
    }

    if (oldsync)
    {
        DBTableTransactionCommitter committer(oldsync->statecachetable);

        // prepare localnodes for a sync change or/and a copy operation
        LocalTreeProcMove tp(parent->sync);
        sync->syncs.proclocaltree(this, &tp);

        // add to new parent map by localname// update local cache if there is a sync change
        oldsync->cachenodes();
        sync->cachenodes();
    }

    if (parent && parentChange)
    {
        LocalTreeProcUpdateTransfers tput;
        sync->syncs.proclocaltree(this, &tput);
    }
}

void LocalNode::moveContentTo(LocalNode* ln, LocalPath& fullPath, bool setScanAgain)
{
    vector<LocalNode*> workingList;
    workingList.reserve(children.size());
    for (auto& c : children) workingList.push_back(c.second);
    for (auto& c : workingList)
    {
        ScopedLengthRestore restoreLen(fullPath);
        fullPath.appendWithSeparator(c->localname, true);
        c->setnameparent(ln, fullPath.leafName(), sync->syncs.fsaccess->fsShortname(fullPath));

        // if moving between syncs, removal from old sync db is already done
        ln->sync->statecacheadd(c);

        if (setScanAgain)
        {
            c->setScanAgain(false, true, true, 0);
        }
    }

    ln->resetTransfer(move(transferSP));

    LocalTreeProcUpdateTransfers tput;
    tput.proc(*sync->syncs.fsaccess, ln);

    ln->mWaitingForIgnoreFileLoad = mWaitingForIgnoreFileLoad;

    // Make sure our exclusion state is recomputed.
    ln->setRecomputeExclusionState(true);
}

// delay uploads by 1.1 s to prevent server flooding while a file is still being written
void LocalNode::bumpnagleds()
{
    nagleds = Waiter::ds + 11;
}

LocalNode::LocalNode(Sync* csync)
: sync(csync)
, scanAgain(TREE_RESOLVED)
, checkMovesAgain(TREE_RESOLVED)
, syncAgain(TREE_RESOLVED)
, conflicts(TREE_RESOLVED)
, unstableFsidAssigned(false)
, deletedFS(false)
, moveApplyingToLocal(false)
, moveAppliedToLocal(false)
, scanInProgress(false)
, scanObsolete(false)
, parentSetScanAgain(false)
, parentSetCheckMovesAgain(false)
, parentSetSyncAgain(false)
, parentSetContainsConflicts(false)
, fsidSyncedReused(false)
, fsidScannedReused(false)
, confirmDeleteCount(0)
, certainlyOrphaned(0)
, neverScanned(0)
{
    fsid_lastSynced_it = sync->syncs.localnodeBySyncedFsid.end();
    fsid_asScanned_it = sync->syncs.localnodeByScannedFsid.end();
    syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.end();
}

// initialize fresh LocalNode object - must be called exactly once
void LocalNode::init(nodetype_t ctype, LocalNode* cparent, const LocalPath& cfullpath, std::unique_ptr<LocalPath> shortname)
{
    parent = NULL;
//    notseen = 0;
    unstableFsidAssigned = false;
    deletedFS = false;
    moveAppliedToLocal = false;
    moveApplyingToLocal = false;
    oneTimeUseSyncedFingerprintInScan = false;
    recomputeFingerprint = false;
    scanAgain = TREE_RESOLVED;
    checkMovesAgain = TREE_RESOLVED;
    syncAgain = TREE_RESOLVED;
    conflicts = TREE_RESOLVED;
    parentSetCheckMovesAgain = false;
    parentSetSyncAgain = false;
    parentSetScanAgain = false;
    parentSetContainsConflicts = false;
    fsidSyncedReused = false;
    fsidScannedReused = false;
    confirmDeleteCount = 0;
    certainlyOrphaned = 0;
    neverScanned = 0;
    scanInProgress = false;
    scanObsolete = false;
    slocalname = NULL;

    if (type != FILENODE)
    {
        neverScanned = 1;
        ++sync->threadSafeState->neverScannedFolderCount;
    }

    mReportedSyncState = TREESTATE_NONE;

    type = ctype;

    bumpnagleds();

    mWaitingForIgnoreFileLoad = false;

    if (cparent)
    {
        setnameparent(cparent, cfullpath.leafName(), std::move(shortname));

        mIsIgnoreFile = type == FILENODE && localname == IGNORE_FILE_NAME;

        mExclusionState = parent->exclusionState(localname, type);
    }
    else
    {
        localname = cfullpath;
        toName_of_localname = localname.toName(*sync->syncs.fsaccess);
        slocalname.reset(shortname && *shortname != localname ? shortname.release() : nullptr);

        mExclusionState = ES_INCLUDED;
    }


//#ifdef DEBUG
//    // double check we were given the right shortname (if the file exists yet)
//    auto fa = sync->client->fsaccess->newfileaccess(false);
//    if (fa->fopen(cfullpath))  // exists, is file
//    {
//        auto sn = sync->client->fsaccess->fsShortname(cfullpath);
//        assert(!localname.empty() &&
//            ((!slocalname && (!sn || localname == *sn)) ||
//                (slocalname && sn && !slocalname->empty() && *slocalname != localname && *slocalname == *sn)));
//    }
//#endif

    sync->syncs.totalLocalNodes++;

    if (type >= 0 && type < int(sync->localnodes.size()))
    {
        sync->localnodes[type]++;
    }
}

LocalNode::RareFields::ScanBlocked::ScanBlocked(PrnGen &rng, const LocalPath& lp, LocalNode* ln)
    : scanBlockedTimer(rng)
    , scanBlockedLocalPath(lp)
    , localNode(ln)
{
    scanBlockedTimer.backoff(Sync::SCANNING_DELAY_DS);
}

auto LocalNode::rare() -> RareFields&
{
    // Rare fields are those that are hardly ever used, and we don't want every LocalNode using more RAM for them all the time.
    // Those rare fields are put in this RareFields struct instead, and LocalNode holds an optional unique_ptr to them
    // Only a tiny subset of the LocalNodes should have populated RareFields at any one time.
    // If any of the rare fields are in use, the struct is present.  trimRareFields() removes the struct when none are in use.
    // This function should be used when one of those field is needed, as it creates the struct if it doesn't exist yet
    // and then returns it.

    if (!rareFields)
    {
        rareFields.reset(new RareFields);
    }
    return *rareFields;
}

auto LocalNode::rareRO() const -> const RareFields&
{
    // RO = read only
    // Use this function when you're not sure if rare fields have been populated, but need to check
    if (!rareFields)
    {
        static RareFields blankFields;
        return blankFields;
    }
    return *rareFields;
}

void LocalNode::trimRareFields()
{
    if (rareFields)
    {
        if (!scanInProgress) rareFields->scanRequest.reset();

        if (!rareFields->scanBlocked &&
            !rareFields->scanRequest &&
            rareFields->movePendingFrom.expired() &&
            !rareFields->movePendingTo &&
            !rareFields->moveFromHere &&
            !rareFields->moveToHere &&
            !rareFields->filterChain &&
            !rareFields->badlyFormedIgnoreFilePath &&
            rareFields->createFolderHere.expired() &&
            rareFields->removeNodeHere.expired() &&
            rareFields->unlinkHere.expired())
        {
            rareFields.reset();
        }
    }
}

unique_ptr<LocalPath> LocalNode::cloneShortname() const
{
    return unique_ptr<LocalPath>(
        slocalname
        ? new LocalPath(*slocalname)
        : nullptr);
}


void LocalNode::setScanAgain(bool doParent, bool doHere, bool doBelow, dstime delayds)
{
    if (doHere && scanInProgress)
    {
        scanObsolete = true;
    }

    auto state = TreeState((doHere?1u:0u) << 1 | (doBelow?1u:0u));

    if (state >= TREE_ACTION_HERE && delayds > 0)
        scanDelayUntil = std::max<dstime>(scanDelayUntil,  Waiter::ds + delayds);

    scanAgain = std::max<TreeState>(scanAgain, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->scanAgain = std::max<TreeState>(p->scanAgain, TREE_DESCENDANT_FLAGGED);
    }

    // for scanning, we only need to set the parent once
    if (parent && doParent)
    {
        parent->scanAgain = std::max<TreeState>(parent->scanAgain, TREE_ACTION_HERE);
        doParent = false;
        parentSetScanAgain = false;
    }
    parentSetScanAgain = parentSetScanAgain || doParent;
}

void LocalNode::setCheckMovesAgain(bool doParent, bool doHere, bool doBelow)
{
    auto state = TreeState((doHere?1u:0u) << 1 | (doBelow?1u:0u));

    checkMovesAgain = std::max<TreeState>(checkMovesAgain, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->checkMovesAgain = std::max<TreeState>(p->checkMovesAgain, TREE_DESCENDANT_FLAGGED);
    }

    parentSetCheckMovesAgain = parentSetCheckMovesAgain || doParent;
}

void LocalNode::setSyncAgain(bool doParent, bool doHere, bool doBelow)
{
    auto state = TreeState((doHere?1u:0u) << 1 | (doBelow?1u:0u));

    syncAgain = std::max<TreeState>(syncAgain, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->syncAgain = std::max<TreeState>(p->syncAgain, TREE_DESCENDANT_FLAGGED);
    }

    parentSetSyncAgain = parentSetSyncAgain || doParent;
}

void LocalNode::setContainsConflicts(bool doParent, bool doHere, bool doBelow)
{
    // using the 3 flags for consistency & understandabilty but doBelow is not relevant
    assert(!doBelow);

    auto state = TreeState((doHere?1u:0u) << 1 | (doBelow?1u:0u));

    conflicts = std::max<TreeState>(conflicts, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->conflicts = std::max<TreeState>(p->conflicts, TREE_DESCENDANT_FLAGGED);
    }

    parentSetContainsConflicts = parentSetContainsConflicts || doParent;
}

void LocalNode::initiateScanBlocked(bool folderBlocked, bool containsFingerprintBlocked)
{

    // Setting node as scan-blocked. The main loop will check it regularly by weak_ptr
    if (!rare().scanBlocked)
    {
        rare().scanBlocked.reset(new RareFields::ScanBlocked(sync->syncs.rng, getLocalPath(), this));
        sync->syncs.scanBlockedPaths.push_back(rare().scanBlocked);
    }

    if (folderBlocked && !rare().scanBlocked->folderUnreadable)
    {
        rare().scanBlocked->folderUnreadable = true;

        LOG_verbose << sync->syncname << "Directory scan has become inaccesible for path: " << getLocalPath();

        // Mark all immediate children as requiring refingerprinting.
        for (auto& childIt : children)
        {
            if (childIt.second->type == FILENODE)
                childIt.second->recomputeFingerprint = true;
        }
    }

    if (containsFingerprintBlocked && !rare().scanBlocked->filesUnreadable)
    {
        LOG_verbose << sync->syncname << "Directory scan contains fingerprint-blocked files: " << getLocalPath();

        rare().scanBlocked->filesUnreadable = true;
    }
}

bool LocalNode::checkForScanBlocked(FSNode* fsNode)
{
    if (rareRO().scanBlocked && rare().scanBlocked->folderUnreadable)
    {
        // Have we recovered?
        if (fsNode && fsNode->type != TYPE_UNKNOWN && !fsNode->isBlocked)
        {
            LOG_verbose << sync->syncname << "Recovered from being scan blocked: " << getLocalPath();

            type = fsNode->type; // original scan may not have been able to discern type, fix it now
            setScannedFsid(UNDEF, sync->syncs.localnodeByScannedFsid, fsNode->localname, FileFingerprint());
            sync->statecacheadd(this);

            if (!rare().scanBlocked->filesUnreadable)
            {
                rare().scanBlocked.reset();
                trimRareFields();
                return false;
            }
        }

        LOG_verbose << sync->syncname << "Waiting on scan blocked timer, retry in ds: "
            << rare().scanBlocked->scanBlockedTimer.retryin() << " for " << getLocalPath();

        // make sure path stays accurate in case this node moves
        rare().scanBlocked->scanBlockedLocalPath = getLocalPath();

        return true;
    }

    if (fsNode && (fsNode->type == TYPE_UNKNOWN || fsNode->isBlocked))
    {
        // We were not able to get details of the filesystem item when scanning the directory.
        // Consider it a blocked file, and we'll rescan the folder from time to time.
        LOG_verbose << sync->syncname << "File/folder was blocked when reading directory, retry later: " << getLocalPath();

        // Setting node as scan-blocked. The main loop will check it regularly by weak_ptr
        initiateScanBlocked(true, false);
        return true;
    }

    return false;
}


bool LocalNode::scanRequired() const
{
    return scanAgain != TREE_RESOLVED;
}

void LocalNode::clearRegeneratableFolderScan(SyncPath& fullPath, vector<syncRow>& childRows)
{
    if (lastFolderScan &&
        lastFolderScan->size() == children.size())
    {
        // check for scan-blocked entries, those are not regeneratable
        for (auto& c : *lastFolderScan)
        {
            if (c.type == TYPE_UNKNOWN) return;
            if (c.isBlocked) return;
        }

        // check that generating the fsNodes results in the same set
        unsigned nChecked = 0;
        for (auto& row : childRows)
        {
            if (!!row.syncNode != !!row.fsNode) return;
            if (row.syncNode && row.fsNode)
            {
                if (row.syncNode->type == FILENODE &&
                    !scannedFingerprint.isvalid)
                {
                    return;
                }

                ++nChecked;
                auto generated = row.syncNode->getScannedFSDetails();
                if (!generated.equivalentTo(*row.fsNode)) return;
            }
        }

        if (nChecked == children.size())
        {
            // LocalNodes are now consistent with the last scan.
            LOG_debug << sync->syncname << "Clearing regeneratable folder scan records (" << lastFolderScan->size() << ") at " << fullPath.localPath;
            lastFolderScan.reset();
        }
    }
}

bool LocalNode::mightHaveMoves() const
{
    return checkMovesAgain != TREE_RESOLVED;
}

bool LocalNode::syncRequired() const
{
    return syncAgain != TREE_RESOLVED;
}


void LocalNode::propagateAnySubtreeFlags()
{
    for (auto& child : children)
    {
        if (child.second->type != FILENODE)
        {
            if (scanAgain == TREE_ACTION_SUBTREE)
            {
                child.second->scanDelayUntil = std::max<dstime>(child.second->scanDelayUntil,  scanDelayUntil);
            }

            child.second->scanAgain = propagateSubtreeFlag(scanAgain, child.second->scanAgain);
            child.second->checkMovesAgain = propagateSubtreeFlag(checkMovesAgain, child.second->checkMovesAgain);
            child.second->syncAgain = propagateSubtreeFlag(syncAgain, child.second->syncAgain);
        }
    }
    if (scanAgain == TREE_ACTION_SUBTREE) scanAgain = TREE_ACTION_HERE;
    if (checkMovesAgain == TREE_ACTION_SUBTREE) checkMovesAgain = TREE_ACTION_HERE;
    if (syncAgain == TREE_ACTION_SUBTREE) syncAgain = TREE_ACTION_HERE;
}

static bool isDoNotSyncFileName(const string& name)
{
    return name == "desktop.ini"
           || name == ".DS_Store"
           || name == "Icon\x0d";
}

bool LocalNode::processBackgroundFolderScan(syncRow& row, SyncPath& fullPath)
{
    bool syncHere = false;

    assert(row.syncNode == this);
    assert(row.fsNode);
    assert(!sync->localdebris.isContainingPathOf(fullPath.localPath));

    std::shared_ptr<ScanService::ScanRequest> ourScanRequest = scanInProgress ? rare().scanRequest  : nullptr;

    std::shared_ptr<ScanService::ScanRequest>* availableScanSlot = nullptr;
    if (!sync->mActiveScanRequestGeneral || sync->mActiveScanRequestGeneral->completed())
    {
        availableScanSlot = &sync->mActiveScanRequestGeneral;
    }
    else if (neverScanned &&
            (!sync->mActiveScanRequestUnscanned || sync->mActiveScanRequestUnscanned->completed()))
    {
        availableScanSlot = &sync->mActiveScanRequestUnscanned;
    }

    if (!ourScanRequest && availableScanSlot)
    {
        // we can start a single new request if we are still recursing and the last request from this sync completed already
        if (scanDelayUntil != 0 && Waiter::ds < scanDelayUntil)
        {
            LOG_verbose << sync->syncname << "Too soon to scan this folder, needs more ds: " << scanDelayUntil - Waiter::ds;
        }
        else
        {
            // queueScan() already logs: LOG_verbose << "Requesting scan for: " << fullPath.toPath(*client->fsaccess);
            scanObsolete = false;
            scanInProgress = true;

            // If enough details of the scan are the same, we can reuse fingerprints instead of recalculating
            map<LocalPath, FSNode> priorScanChildren;

            if (lastFolderScan)
            {
                // use the same fingerprint shortcut data as the last time we scanned,
                // if we still have it (including fingerprint isvalid flag)
                for (auto& f : *lastFolderScan)
                {
                    if (f.type == FILENODE && f.fingerprint.isvalid)
                    {
                        priorScanChildren.emplace(f.localname, f.clone());
                    }
                }
            }

            for (auto& childIt : children)
            {
                auto& child = *childIt.second;

                bool useSyncedFP = child.oneTimeUseSyncedFingerprintInScan;
                child.oneTimeUseSyncedFingerprintInScan = false;

                bool forceRecompute = child.recomputeFingerprint;
                child.recomputeFingerprint = false;

                // Can't fingerprint directories.
                if (child.type != FILENODE || forceRecompute)
                {
                    priorScanChildren.erase(child.localname);
                    continue;
                }

                if (priorScanChildren.find(child.localname) != priorScanChildren.end())
                {
                    // already using not yet discarded last-scan data
                    continue;
                }

                if (child.scannedFingerprint.isvalid)
                {
                    // as-scanned by this instance is more accurate if available
                    priorScanChildren.emplace(childIt.first, child.getScannedFSDetails());
                }
                else if (useSyncedFP && child.fsid_lastSynced != UNDEF && child.syncedFingerprint.isvalid)
                {
                    // But otherwise, already-synced syncs on startup should not re-fingerprint
                    // files that match the synced fingerprint by fsid/size/mtime (for quick startup)
                    priorScanChildren.emplace(childIt.first, child.getLastSyncedFSDetails());
                }
            }

            ourScanRequest = sync->syncs.mScanService->queueScan(fullPath.localPath,
                row.fsNode->fsid, false, move(priorScanChildren));

            rare().scanRequest = ourScanRequest;
            *availableScanSlot = ourScanRequest;

            LOG_verbose << sync->syncname << "Issuing Directory scan request for : " << fullPath.localPath << (availableScanSlot == &sync->mActiveScanRequestUnscanned ? " (in unscanned slot)" : "");

            if (neverScanned)
            {
                neverScanned = 0;
                --sync->threadSafeState->neverScannedFolderCount;
                LOG_verbose << sync->syncname << "Remaining known unscanned folders: " << sync->threadSafeState->neverScannedFolderCount.load();
            }
        }
    }
    else if (ourScanRequest &&
             ourScanRequest->completed())
    {
        if (ourScanRequest == sync->mActiveScanRequestGeneral) sync->mActiveScanRequestGeneral.reset();
        if (ourScanRequest == sync->mActiveScanRequestUnscanned) sync->mActiveScanRequestUnscanned.reset();

        scanInProgress = false;

        if (SCAN_FSID_MISMATCH == ourScanRequest->completionResult())
        {
            LOG_verbose << sync->syncname << "Directory scan detected outdated fsid : " << fullPath.localPath;
            scanObsolete = true;
        }

        if (SCAN_SUCCESS == ourScanRequest->completionResult()
            && ourScanRequest->fsidScanned() != row.fsNode->fsid)
        {
            LOG_verbose << sync->syncname << "Directory scan returned was for now outdated fsid : " << fullPath.localPath;
            scanObsolete = true;
        }

        if (scanObsolete)
        {
            LOG_verbose << sync->syncname << "Directory scan outdated for : " << fullPath.localPath;
            scanObsolete = false;

            // Scan results are out of date but may still be useful.
            lastFolderScan.reset(new vector<FSNode>(ourScanRequest->resultNodes()));

            // Mark this directory as requiring another scan.
            setScanAgain(false, true, false, 10);
        }
        else if (SCAN_SUCCESS == ourScanRequest->completionResult())
        {
            lastFolderScan.reset(new vector<FSNode>(ourScanRequest->resultNodes()));

            for (auto& i : *lastFolderScan)
            {
                if (isDoNotSyncFileName(i.localname.toPath(true)))
                {
                    // These are special shell-generated files for win & mac, only relevant in the filesystem they were created
                    i.type = TYPE_DONOTSYNC;
                }
            }

            LOG_verbose << sync->syncname << "Received " << lastFolderScan->size() << " directory scan results for: " << fullPath.localPath;

            scanDelayUntil = Waiter::ds + 20; // don't scan too frequently
            scanAgain = TREE_RESOLVED;
            setSyncAgain(false, true, false);
            syncHere = true;

            size_t numFingerprintBlocked = 0;
            for (auto& n : *lastFolderScan)
            {
                if (n.type == FILENODE && !n.fingerprint.isvalid)
                {
                    LOG_debug << "Directory scan contains a file that could not be fingerprinted: " << n.localname;
                    ++numFingerprintBlocked;
                }
            }

            if (numFingerprintBlocked)
            {
                initiateScanBlocked(false, true);
            }
            else if (rareRO().scanBlocked &&
                     rareRO().scanBlocked->filesUnreadable)
            {
                LOG_verbose << sync->syncname << "Directory scan fingerprint-blocked files all resolved at: " << getLocalPath();
                rare().scanBlocked.reset();
                trimRareFields();
            }
        }
        else // SCAN_INACCESSIBLE
        {
            // we were previously able to scan this node, but now we can't.
            row.fsNode->isBlocked = true;
            if (!checkForScanBlocked(row.fsNode))
            {
                initiateScanBlocked(true, false);
            }
        }
    }

    trimRareFields();
    return syncHere;
}

void LocalNode::reassignUnstableFsidsOnceOnly(const FSNode* fsnode)
{
    if (!sync->fsstableids && !unstableFsidAssigned)
    {
        // for FAT and other filesystems where we can't rely on fsid
        // being the same after remount, so update our previously synced nodes
        // with the actual fsids now attached to them (usually generated by FUSE driver)

        if (fsid_lastSynced != UNDEF)
        {
            auto fsid = UNDEF - 1;
            auto sname = unique_ptr<LocalPath>();

            if (fsnode)
            {
                if (sync->syncEqual(*fsnode, *this))
                    fsid = fsnode->fsid;

                sname = fsnode->cloneShortname();
            }

            setSyncedFsid(fsid, sync->syncs.localnodeBySyncedFsid, localname, std::move(sname));
            sync->statecacheadd(this);
        }

        unstableFsidAssigned = true;
    }
}

void LocalNode::recursiveSetAndReportTreestate(treestate_t ts, bool recurse, bool reportToApp)
{
    if (reportToApp && ts != mReportedSyncState)
    {
        assert(sync->syncs.onSyncThread());
        sync->syncs.mClient.app->syncupdate_treestate(sync->getConfig(), getLocalPath(), ts, type);
    }

    mReportedSyncState = ts;

    if (recurse)
    {
        for (auto& i : children)
        {
            i.second->recursiveSetAndReportTreestate(ts, recurse, reportToApp);
        }
    }
}

treestate_t LocalNode::checkTreestate(bool notifyChangeToApp)
{
    // notify file explorer if the sync state overlay icon should change

    treestate_t ts = TREESTATE_NONE;

    if (scanAgain == TREE_RESOLVED &&
        checkMovesAgain == TREE_RESOLVED &&
        syncAgain == TREE_RESOLVED)
    {
        ts = TREESTATE_SYNCED;
    }
    else if (type == FILENODE)
    {
        ts = TREESTATE_PENDING;
    }
    else if (scanAgain <= TREE_DESCENDANT_FLAGGED &&
        checkMovesAgain <= TREE_DESCENDANT_FLAGGED &&
        syncAgain <= TREE_DESCENDANT_FLAGGED)
    {
        ts = TREESTATE_SYNCING;
    }
    else
    {
        ts = TREESTATE_PENDING;
    }

    recursiveSetAndReportTreestate(ts, false, notifyChangeToApp);

    return ts;
}


// set fsid - assume that an existing assignment of the same fsid is no longer current and revoke
void LocalNode::setSyncedFsid(handle newfsid, fsid_localnode_map& fsidnodes, const LocalPath& fsName, std::unique_ptr<LocalPath> newshortname)
{
    if (fsid_lastSynced_it != fsidnodes.end())
    {
        if (newfsid == fsid_lastSynced && localname == fsName)
        {
            return;
        }

        fsidnodes.erase(fsid_lastSynced_it);
    }

    fsid_lastSynced = newfsid;
    fsidSyncedReused = false;

    // if synced to fs, localname should match exactly (no differences in case/escaping etc)
    if (localname != fsName ||
            !!newshortname != !!slocalname ||
            (newshortname && slocalname && *newshortname != *slocalname))
    {
        // localname must always be set by this function, to maintain parent's child maps
        setnameparent(parent, fsName, move(newshortname));
    }

    // LOG_verbose << "localnode " << this << " fsid " << toHandle(fsid_lastSynced) << " localname " << fsName.toPath() << " parent " << parent;

    if (fsid_lastSynced == UNDEF)
    {
        fsid_lastSynced_it = fsidnodes.end();
    }
    else
    {
        fsid_lastSynced_it = fsidnodes.insert(std::make_pair(fsid_lastSynced, this));
    }

//    assert(localname.empty() || name.empty() || (!parent && parent_dbid == UNDEF) || parent_dbid == 0 ||
//        0 == compareUtf(localname, true, name, false, true));
}

void LocalNode::setScannedFsid(handle newfsid, fsid_localnode_map& fsidnodes, const LocalPath& fsName, const FileFingerprint& scanfp)
{
    if (fsid_asScanned_it != fsidnodes.end())
    {
        fsidnodes.erase(fsid_asScanned_it);
    }

    fsid_asScanned = newfsid;
    fsidScannedReused = false;

    scannedFingerprint = scanfp;

    if (fsid_asScanned == UNDEF)
    {
        fsid_asScanned_it = fsidnodes.end();
    }
    else
    {
        fsid_asScanned_it = fsidnodes.insert(std::make_pair(fsid_asScanned, this));
    }

    assert(fsid_asScanned == UNDEF || 0 == compareUtf(localname, true, fsName, true, true));
}

void LocalNode::setSyncedNodeHandle(NodeHandle h)
{
    if (syncedCloudNodeHandle_it != sync->syncs.localnodeByNodeHandle.end())
    {
        if (h == syncedCloudNodeHandle)
        {
            return;
        }

        assert(syncedCloudNodeHandle_it->first == syncedCloudNodeHandle);

        // too verbose for million-node syncs
        //LOG_verbose << sync->syncname << "removing synced handle " << syncedCloudNodeHandle << " for " << localnodedisplaypath(*sync->syncs.fsaccess);

        sync->syncs.localnodeByNodeHandle.erase(syncedCloudNodeHandle_it);
    }

    syncedCloudNodeHandle = h;

    if (syncedCloudNodeHandle == UNDEF)
    {
        syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.end();
    }
    else
    {
        // too verbose for million-node syncs
        //LOG_verbose << sync->syncname << "adding synced handle " << syncedCloudNodeHandle << " for " << localnodedisplaypath(*sync->syncs.fsaccess);

        syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.insert(std::make_pair(syncedCloudNodeHandle, this));
    }

//    assert(localname.empty() || name.empty() || (!parent && parent_dbid == UNDEF) || parent_dbid == 0 ||
//        0 == compareUtf(localname, true, name, false, true));
}

LocalNode::~LocalNode()
{
    if (!sync->mDestructorRunning && dbid)
    {
        sync->statecachedel(this);
    }

    if (sync->dirnotify && !sync->mDestructorRunning)
    {
        // deactivate corresponding notifyq records
        sync->dirnotify->fsEventq.replaceLocalNodePointers(this, (LocalNode*)~0);
    }

    if (!sync->syncs.mExecutingLocallogout)
    {
        // for Locallogout, we will resume syncs and their transfers on re-login.
        // for other cases - single sync cancel, disable etc - transfers are cancelled.
        resetTransfer(nullptr);

        sync->syncs.mMoveInvolvedLocalNodes.erase(this);

        // remove from fsidnode map, if present
        if (fsid_lastSynced_it != sync->syncs.localnodeBySyncedFsid.end())
        {
            sync->syncs.localnodeBySyncedFsid.erase(fsid_lastSynced_it);
        }
        if (fsid_asScanned_it != sync->syncs.localnodeByScannedFsid.end())
        {
            sync->syncs.localnodeByScannedFsid.erase(fsid_asScanned_it);
        }
        if (syncedCloudNodeHandle_it != sync->syncs.localnodeByNodeHandle.end())
        {
            sync->syncs.localnodeByNodeHandle.erase(syncedCloudNodeHandle_it);
        }
    }

    sync->syncs.totalLocalNodes--;

    if (type >= 0 && type < int(sync->localnodes.size()))
    {
        sync->localnodes[type]--;
    }

    // remove parent association
    if (parent)
    {
        setnameparent(nullptr, LocalPath(), nullptr);
    }

    deleteChildren();
}

void LocalNode::deleteChildren()
{
    for (localnode_map::iterator it = children.begin(); it != children.end(); )
    {
        // the destructor removes the child from our `children` map
        delete it++->second;
    }
    assert(children.empty());
}


bool LocalNode::conflictsDetected() const
{
    return conflicts != TREE_RESOLVED;
}

bool LocalNode::isAbove(const LocalNode& other) const
{
    return other.isBelow(*this);
}

bool LocalNode::isBelow(const LocalNode& other) const
{
    for (auto* node = parent; node; node = node->parent)
    {
        if (node == &other)
        {
            return true;
        }
    }

    return false;
}

void LocalNode::setSubtreeNeedsRefingerprint()
{
    // Re-calculate fingerprints on disk
    // setScanAgain should be called separately
    recomputeFingerprint = true;
    oneTimeUseSyncedFingerprintInScan = false;

    for (auto& child : children)
    {
        if (type != FILENODE)  // no need to set it for file versions
        {
            child.second->setSubtreeNeedsRefingerprint();
        }
    }
}

LocalPath LocalNode::getLocalPath() const
{
    LocalPath lp;
    getlocalpath(lp);
    return lp;
}

void LocalNode::getlocalpath(LocalPath& path) const
{
    path.clear();

    for (const LocalNode* l = this; l != nullptr; l = l->parent)
    {
        assert(!l->parent || l->parent->sync == sync);

        // sync root has absolute path, the rest are just their leafname
        path.prependWithSeparator(l->localname);
    }
}

string LocalNode::getCloudPath(bool guessLeafName) const
{
    // We may need to guess the leaf name if we suspect
    // or know that the corresponding cloud node has been moved/renamed
    // and we need its old name

    string path;

    const LocalNode* l = this;

    if (guessLeafName)
    {
        path = l->localname.toName(*sync->syncs.fsaccess);
        l = l->parent;
    }

    for (; l != nullptr; l = l->parent)
    {
        string name;

        CloudNode cn;
        string fullpath;
        if (sync->syncs.lookupCloudNode(l->syncedCloudNodeHandle, cn, l->parent ? nullptr : &fullpath,
            nullptr, nullptr, nullptr, nullptr, Syncs::LATEST_VERSION))
        {
            name = cn.name;
        }
        else
        {
            name = l->localname.toName(*sync->syncs.fsaccess);
        }

        assert(!l->parent || l->parent->sync == sync);

        if (!path.empty())
            path.insert(0, 1, '/');

        path.insert(0, l->parent ? name : fullpath);
    }
    return path;
}


string LocalNode::debugGetParentList()
{
    string s;

    for (const LocalNode* l = this; l != nullptr; l = l->parent)
    {
        s += l->localname.toPath(false) + "(" + std::to_string((long long)(void*)l) + ") ";
    }
    return s;
}

// locate child by localname or slocalname
LocalNode* LocalNode::childbyname(LocalPath* localname)
{
    localnode_map::iterator it;

    if (!localname || ((it = children.find(*localname)) == children.end() && (it = schildren.find(*localname)) == schildren.end()))
    {
        return NULL;
    }

    return it->second;
}

LocalNode* LocalNode::findChildWithSyncedNodeHandle(NodeHandle h)
{
    for (auto& c : children)
    {
        if (c.second->syncedCloudNodeHandle == h)
        {
            return c.second;
        }
    }
    return nullptr;
}

FSNode LocalNode::getLastSyncedFSDetails() const
{
    assert(fsid_lastSynced != UNDEF);

    FSNode n;
    n.localname = localname;
    n.shortname = slocalname ? make_unique<LocalPath>(*slocalname): nullptr;
    n.type = type;
    n.fsid = fsid_lastSynced;
    n.isSymlink = false;  // todo: store localndoes for symlinks but don't use them?
    n.fingerprint = syncedFingerprint;
    assert(syncedFingerprint.isvalid || type != FILENODE);
    return n;
}


FSNode LocalNode::getScannedFSDetails() const
{
    FSNode n;
    n.localname = localname;
    n.shortname = slocalname ? make_unique<LocalPath>(*slocalname): nullptr;
    n.type = type;
    n.fsid = fsid_asScanned;
    n.isSymlink = false;  // todo: store localndoes for symlinks but don't use them?
    n.fingerprint = scannedFingerprint;
    assert(scannedFingerprint.isvalid || type != FILENODE);
    return n;
}

void LocalNode::updateMoveInvolvement()
{
    bool moveInvolved = hasRare() && (rare().moveToHere || rare().moveFromHere);
    if (moveInvolved)
    {
        sync->syncs.mMoveInvolvedLocalNodes.insert(this);
    }
    else
    {
        sync->syncs.mMoveInvolvedLocalNodes.erase(this);
    }
}

void LocalNode::queueClientUpload(shared_ptr<SyncUpload_inClient> upload, VersioningOption vo, bool queueFirst)
{
    resetTransfer(upload);

    sync->syncs.queueClient([upload, vo, queueFirst](MegaClient& mc, TransferDbCommitter& committer)
        {
            upload->transferTag = mc.nextreqtag();
            upload->selfKeepAlive = upload;
            mc.startxfer(PUT, upload.get(), committer, false, queueFirst, false, vo);
        });

}

void LocalNode::queueClientDownload(shared_ptr<SyncDownload_inClient> download, bool queueFirst)
{
    resetTransfer(download);

    sync->syncs.queueClient([download, queueFirst](MegaClient& mc, TransferDbCommitter& committer)
        {
            mc.nextreqtag();
            download->selfKeepAlive = download;
            mc.startxfer(GET, download.get(), committer, false, queueFirst, false, NoVersioning);
        });

}

void LocalNode::resetTransfer(shared_ptr<SyncTransfer_inClient> p)
{
    if (transferSP)
    {
        if (!transferSP->wasTerminated &&
            !transferSP->wasCompleted)
        {
            LOG_debug << "Abandoning old transfer, and queueing its cancel on client thread";

            // this flag allows in-progress transfers to self-cancel
            transferSP->wasRequesterAbandoned = true;

            // also queue an operation on the client thread to cancel it if it's queued
            auto tsp = transferSP;
            sync->syncs.queueClient([tsp](MegaClient& mc, TransferDbCommitter& committer)
                {
                    mc.nextreqtag();
                    mc.stopxfer(tsp.get(), &committer);
                });
        }
    }

    transferSP = move(p);
}


void LocalNode::updateTransferLocalname()
{
    if (transferSP)
    {
        transferSP->setLocalname(getLocalPath());
    }
}

void LocalNode::transferResetUnlessMatched(direction_t dir, const FileFingerprint& fingerprint)
{
    if (!transferSP)
        return;

    auto uploadPtr = dynamic_cast<SyncUpload_inClient*>(transferSP.get());

    auto different =
      dir != (uploadPtr ? PUT : GET)
      || transferSP->fingerprint() != fingerprint;

    // todo: should we be more accurate than just fingerprint?
    if (different || (transferSP->wasTerminated && transferSP->mError != API_EKEY))
    {
        if (uploadPtr && uploadPtr->putnodesStarted)
        {
            // checking for a race where we already sent putnodes and it hasn't completed,
            // then we discover something that means we should abandon the transfer
            LOG_debug << sync->syncname << "Cancelling superceded transfer even though we have an outstanding putnodes request! " << transferSP->getLocalname();
            assert(false);
        }

        LOG_debug << sync->syncname << "Cancelling superceded transfer of " << transferSP->getLocalname();
        resetTransfer(nullptr);
    }
}

void SyncTransfer_inClient::terminated(error e)
{
    File::terminated(e);

    if (e == API_EOVERQUOTA)
    {
        syncThreadSafeState->client()->syncs.disableSyncByBackupId(syncThreadSafeState->backupId(), FOREIGN_TARGET_OVERSTORAGE, false, true, nullptr);
    }

    wasTerminated = true;
    selfKeepAlive.reset();  // deletes this object! (if abandoned by sync)
}

void SyncTransfer_inClient::completed(Transfer* t, putsource_t source)
{
    assert(source == PUTNODES_SYNC);

    // do not allow the base class to submit putnodes immediately
    //File::completed(t, source);

    wasCompleted = true;
    selfKeepAlive.reset();  // deletes this object! (if abandoned by sync)
}

void SyncUpload_inClient::completed(Transfer* t, putsource_t source)
{
    // Keep the info required for putnodes and wait for
    // the sync thread to validate and activate the putnodes

    uploadHandle = t->uploadhandle;
    uploadToken = *t->ultoken;
    fileNodeKey = t->filekey;

    SyncTransfer_inClient::completed(t, source);
}

void SyncUpload_inClient::sendPutnodes(MegaClient* client, NodeHandle ovHandle)
{
    // Always called from the client thread
    weak_ptr<SyncThreadsafeState> stts = syncThreadSafeState;

    // So we know whether it's safe to update putnodesCompleted.
    weak_ptr<SyncUpload_inClient> self = shared_from_this();

    // since we are now sending putnodes, no need to remember puts to inform the client on abandonment
    syncThreadSafeState->client()->transferBackstop.forget(transferTag);

    File::sendPutnodes(client,
        uploadHandle,
        uploadToken,
        fileNodeKey,
        PUTNODES_SYNC,
        ovHandle,
        [self, stts](const Error& e, targettype_t t, vector<NewNode>& nn, bool targetOverride){
            // Is the originating transfer still alive?
            if (auto s = self.lock())
            {
                // Then track the result of its putnodes request.
                s->putnodesFailed = e != API_OK;

                // Capture the handle if the putnodes was successful.
                if (!s->putnodesFailed)
                    s->putnodesResultHandle = nn.front().mAddedHandle;

                // Let the engine know the putnodes has completed.
                s->wasPutnodesCompleted.store(true);
            }

            if (auto s = stts.lock())
            {
                auto client = s->client();
                if (e == API_EACCESS)
                {
                    client->sendevent(99402, "API_EACCESS putting node in sync transfer", 0);
                }
                else if (e == API_EOVERQUOTA)
                {
                    client->syncs.disableSyncByBackupId(s->backupId(),  FOREIGN_TARGET_OVERSTORAGE, false, true, nullptr);
                }

                // since we used a completion function, putnodes_result is not called.
                // but the intermediate layer still needs that in order to call the client app back:
                client->app->putnodes_result(e, t, nn, targetOverride);
            }
        }, nullptr, syncThreadSafeState->mCanChangeVault);
}

SyncUpload_inClient::SyncUpload_inClient(NodeHandle targetFolder, const LocalPath& fullPath,
        const string& nodeName, const FileFingerprint& ff, shared_ptr<SyncThreadsafeState> stss,
        handle fsid, const LocalPath& localname, bool fromInshare)
{
    *static_cast<FileFingerprint*>(this) = ff;

    // normalized name (UTF-8 with unescaped special chars)
    // todo: we did unescape them though?
    name = nodeName;

    // setting the full path means it works like a normal non-sync transfer
    setLocalname(fullPath);

    h = targetFolder;

    hprivate = false;
    hforeign = false;
    syncxfer = true;
    fromInsycShare = fromInshare;
    temporaryfile = false;
    chatauth = nullptr;
    transfer = nullptr;
    tag = 0;

    syncThreadSafeState = move(stss);
    syncThreadSafeState->transferBegin(PUT, size);

    sourceFsid = fsid;
    sourceLocalname = localname;
}

SyncUpload_inClient::~SyncUpload_inClient()
{
    if (!wasTerminated && !wasCompleted)
    {
        assert(wasRequesterAbandoned);
        transfer = nullptr;  // don't try to remove File from Transfer from the wrong thread
    }

    if (wasCompleted && wasPutnodesCompleted)
    {
        syncThreadSafeState->transferComplete(PUT, size);
    }
    else
    {
        syncThreadSafeState->transferFailed(PUT, size);
    }

    if (putnodesStarted)
    {
        syncThreadSafeState->removeExpectedUpload(h, name);
    }
}

void SyncUpload_inClient::prepare(FileSystemAccess&)
{
    transfer->localfilename = getLocalname();

    // is this transfer in progress? update file's filename.
    if (transfer->slot && transfer->slot->fa && !transfer->slot->fa->nonblocking_localname.empty())
    {
        transfer->slot->fa->updatelocalname(transfer->localfilename, false);
    }

    //todo: localNode.treestate(TREESTATE_SYNCING);
}


// serialize/unserialize the following LocalNode properties:
// - type/size
// - fsid
// - parent LocalNode's dbid
// - corresponding Node handle
// - local name
// - fingerprint crc/mtime (filenodes only)
bool LocalNodeCore::write(string& destination, uint32_t parentID)
{
    // We need size even if we're not synced.
    auto size = syncedFingerprint.isvalid ? syncedFingerprint.size : 0;

    CacheableWriter w(destination);
    w.serializei64(type ? -type : size);
    w.serializehandle(fsid_lastSynced);
    w.serializeu32(parentID);
    w.serializenodehandle(syncedCloudNodeHandle.as8byte());
    w.serializestring(localname.platformEncoded());
    if (type == FILENODE)
    {
        if (syncedFingerprint.isvalid)
        {
            w.serializebinary((byte*)syncedFingerprint.crc.data(), sizeof(syncedFingerprint.crc));
            w.serializecompressedi64(syncedFingerprint.mtime);
        }
        else
        {
            static FileFingerprint zeroFingerprint;
            w.serializebinary((byte*)zeroFingerprint.crc.data(), sizeof(zeroFingerprint.crc));
            w.serializecompressedi64(zeroFingerprint.mtime);
        }
    }

    // Formerly mSyncable.
    //
    // No longer meaningful but serialized to maintain compatibility.
    w.serializebyte(1u);

    // first flag indicates we are storing slocalname.
    // Storing it is much, much faster than looking it up on startup.
    w.serializeexpansionflags(1, 1);
    auto tmpstr = slocalname ? slocalname->platformEncoded() : string();
    w.serializepstr(slocalname ? &tmpstr : nullptr);

    w.serializebool(namesSynchronized);

    return true;
}

bool LocalNode::serialize(string* d)
{
    assert(type != TYPE_UNKNOWN);

    // In fact this can occur, eg we invalidated scannedFingerprint when it was below a removed node, when an ancestor folder moved
    // Or (probably) from a node created from the cloud only
    //assert(type != FILENODE || syncedFingerprint.isvalid || scannedFingerprint.isvalid);

    // Every node we serialize should have a parent.
    assert(parent);

    // The only node with a zero DBID should be the root.
    assert(parent->dbid || !parent->parent);

#ifdef DEBUG
    if (fsid_lastSynced != UNDEF)
    {
        LocalPath localpath = getLocalPath();
        auto fa = sync->syncs.fsaccess->newfileaccess(false);
        if (fa->fopen(localpath))  // exists, is file
        {
            auto sn = sync->syncs.fsaccess->fsShortname(localpath);
            if (!(!localname.empty() &&
                ((!slocalname && (!sn || localname == *sn)) ||
                    (slocalname && sn && !slocalname->empty() && *slocalname != localname && *slocalname == *sn))))
            {
                // we can't assert here or it can cause test failures, when the LocalNode just hasn't been updated from the disk state yet.
                // but we can log ERR to try to detect any issues during development.  Occasionally there will be false positives,
                // but also please do investigate when it's not a test that got shut down while busy.
                LOG_err << "Shortname mismatch on LocalNode serialize! " <<
                           "localname: " << localname << " slocalname " << (slocalname?*slocalname:LocalPath()) << (slocalname?"":"<null>") <<
                           " actual shorname " << (sn?*sn:LocalPath()) << (sn?"":"<null>") << " for path " << localpath;

            }
        }
    }
#endif

    auto parentID = parent ? parent->dbid : 0;
    auto result = LocalNodeCore::write(*d, parentID);

#ifdef DEBUG
    // Quick (de)serizliation check.
    {
        string source = *d;
        uint32_t id = 0u;

        auto node = unserialize(*sync, source, id);

        assert(node);
        assert(node->localname == localname);
        assert(!node->slocalname == !slocalname);
        assert(!node->slocalname || *node->slocalname == *slocalname);
    }
#endif

    return result;
}

bool LocalNodeCore::read(const string& source, uint32_t& parentID)
{
    if (source.size() < sizeof(m_off_t)         // type/size combo
                      + sizeof(handle)          // fsid
                      + sizeof(uint32_t)        // parent dbid
                      + MegaClient::NODEHANDLE  // handle
                      + sizeof(short))          // localname length
    {
        LOG_err << "LocalNode unserialization failed - short data";
        return false;
    }

    CacheableReader r(source);

    nodetype_t type;
    m_off_t size;

    if (!r.unserializei64(size)) return false;

    if (size < 0 && size >= -FOLDERNODE)
    {
        // will any compiler optimize this to a const assignment?
        type = (nodetype_t)-size;
        size = 0;
    }
    else
    {
        type = FILENODE;
    }

    handle fsid;
    handle h = 0;
    string localname, shortname;
    m_time_t mtime = 0;
    int32_t crc[4];
    memset(crc, 0, sizeof crc);
    byte syncable = 1;
    unsigned char expansionflags[8] = { 0 };
    bool ns = false;

    if (!r.unserializehandle(fsid) ||
        !r.unserializeu32(parentID) ||
        !r.unserializenodehandle(h) ||
        !r.unserializestring(localname) ||
        (type == FILENODE && !r.unserializebinary((byte*)crc, sizeof(crc))) ||
        (type == FILENODE && !r.unserializecompressedi64(mtime)) ||
        (r.hasdataleft() && !r.unserializebyte(syncable)) ||
        (r.hasdataleft() && !r.unserializeexpansionflags(expansionflags, 2)) ||
        (expansionflags[0] && !r.unserializecstr(shortname, false)) ||
        (expansionflags[1] && !r.unserializebool(ns)))
    {
        LOG_err << "LocalNode unserialization failed at field " << r.fieldnum;
        assert(false);
        return false;
    }
    assert(!r.hasdataleft());

    this->type = type;
    this->syncedFingerprint.size = size;
    this->fsid_lastSynced = fsid;
    this->localname = LocalPath::fromPlatformEncodedRelative(localname);
    this->slocalname.reset(shortname.empty() ? nullptr : new LocalPath(LocalPath::fromPlatformEncodedRelative(shortname)));
    this->slocalname_in_db = 0 != expansionflags[0];
    this->namesSynchronized = ns;

    memcpy(this->syncedFingerprint.crc.data(), crc, sizeof crc);

    this->syncedFingerprint.mtime = mtime;
    this->syncedFingerprint.isvalid = mtime != 0;

    // previously we scanned and created the LocalNode, but we had not set syncedFingerprint
    this->syncedCloudNodeHandle.set6byte(h);

    return true;
}

unique_ptr<LocalNode> LocalNode::unserialize(Sync& sync, const string& source, uint32_t& parentID)
{
    auto node = ::mega::make_unique<LocalNode>(&sync);

    if (!node->read(source, parentID))
        return nullptr;

    return node;
}

#ifdef USE_INOTIFY

LocalNode::WatchHandle::WatchHandle()
  : mEntry(mSentinel.end())
{
}

LocalNode::WatchHandle::~WatchHandle()
{
    operator=(nullptr);
}

auto LocalNode::WatchHandle::operator=(WatchMapIterator entry) -> WatchHandle&
{
    if (mEntry == entry) return *this;

    operator=(nullptr);
    mEntry = entry;

    return *this;
}

auto LocalNode::WatchHandle::operator=(std::nullptr_t) -> WatchHandle&
{
    if (mEntry == mSentinel.end()) return *this;

    auto& node = *mEntry->second.first;
    auto& sync = *node.sync;
    auto& notifier = static_cast<LinuxDirNotify&>(*sync.dirnotify);

    notifier.removeWatch(mEntry);
    invalidate();
    return *this;
}

void LocalNode::WatchHandle::invalidate()
{
    mEntry = mSentinel.end();
}

bool LocalNode::WatchHandle::operator==(handle fsid) const
{
    if (mEntry == mSentinel.end()) return false;

    return fsid == mEntry->second.second;
}

WatchResult LocalNode::watch(const LocalPath& path, handle fsid)
{
    // Can't add a watch if we don't have a notifier.
    if (!sync->dirnotify)
        return WR_SUCCESS;

    // Do we need to (re)create a watch?
    if (mWatchHandle == fsid)
    {
        LOG_verbose << "Watch for path: " << path
                    << " with mWatchHandle == fsid == " << fsid
                    << " Already in place";
        return WR_SUCCESS;
    }

    // Get our hands on the notifier.
    auto& notifier = static_cast<LinuxDirNotify&>(*sync->dirnotify);

    // Add the watch.
    auto result = notifier.addWatch(*this, path, fsid);

    // Were we able to add the watch?
    if (result.second)
    {
        // Yup so assign the handle.
        mWatchHandle = result.first;
    }
    else
    {
        // Make sure any existing watch is invalidated.
        mWatchHandle = nullptr;
    }

    return result.second;
}

WatchMap LocalNode::WatchHandle::mSentinel;

#else // USE_INOTIFY

WatchResult LocalNode::watch(const LocalPath&, handle)
{
    // Only inotify requires us to create watches for each node.
    return WR_SUCCESS;
}

#endif // ! USE_INOTIFY

void LocalNode::clearFilters()
{
    // Only for directories.
    assert(type == FOLDERNODE);

    // Clear filter state.
    if (rareRO().filterChain)
    {
        rare().filterChain.reset();
        rare().badlyFormedIgnoreFilePath.reset();
        trimRareFields();
    }

    // Reset ignore file state.
    setRecomputeExclusionState(false);

    // Re-examine this subtree.
    setScanAgain(false, true, true, 0);
    setSyncAgain(false, true, true);
}

const FilterChain& LocalNode::filterChainRO() const
{
    static const FilterChain dummy;

    auto& filterChainPtr = rareRO().filterChain;

    if (filterChainPtr)
        return *filterChainPtr;

    return dummy;
}

bool LocalNode::loadFiltersIfChanged(const FileFingerprint& fingerprint, const LocalPath& path)
{
    // Only meaningful for directories.
    assert(type == FOLDERNODE);

    // Convenience.
    auto& filterChain = this->filterChain();

    if (filterChain.isValid() && !filterChain.changed(fingerprint))
    {
        return true;
    }

    if (filterChain.isValid())
    {
        filterChain.invalidate();
        setRecomputeExclusionState(false);
    }

    // Try and load the ignore file.
    if (FLR_SUCCESS != filterChain.load(*sync->syncs.fsaccess, path))
    {
        filterChain.invalidate();
    }

    return filterChain.isValid();
}

FilterChain& LocalNode::filterChain()
{
    auto& filterChainPtr = rare().filterChain;

    if (!filterChainPtr)
        filterChainPtr.reset(new FilterChain());

    return *filterChainPtr;
}

bool LocalNode::isExcluded(RemotePathPair namePath, nodetype_t type, bool inherited) const
{
    // This specialization only makes sense for directories.
    assert(this->type == FOLDERNODE);

    // Check whether the file is excluded by any filters.
    for (auto* node = this; node; node = node->parent)
    {
        assert(node->mExclusionState == ES_INCLUDED);

        if (node->rareRO().filterChain)
        {
            // Should we only consider inheritable filter rules?
            inherited = inherited || node != this;

            // Check for a filter match.
            auto result = node->filterChainRO().match(namePath, type, inherited);

            // Was the file matched by any filters?
            if (result.matched)
                return !result.included;
        }

        // Update path so that it's applicable to the next node's path filters.
        namePath.second.prependWithSeparator(node->toName_of_localname);
    }

    // File's included.
    return false;
}

bool LocalNode::isExcluded(const RemotePathPair&, m_off_t size) const
{
    // Specialization only meaningful for directories.
    assert(type == FOLDERNODE);

    // Consider files of unknown size included.
    if (size < 0)
        return false;

    // Check whether this file is excluded by any size filters.
    for (auto* node = this; node; node = node->parent)
    {
        // Sanity: We should never be called if either of these is true.
        assert(node->mExclusionState == ES_INCLUDED);

        if (node->rareRO().filterChain)
        {
            // Check for a filter match.
            auto result = node->filterChainRO().match(size);

            // Was the file matched by any filters?
            if (result.matched)
                return !result.included;
        }
    }

    // File's included.
    return false;
}

//void LocalNode::setWaitingForIgnoreFileLoad(bool pending)
//{
//    // Only meaningful for directories.
//    assert(type == FOLDERNODE);
//
//    // Do we really need to update our children?
//    if (!mWaitingForIgnoreFileLoad)
//    {
//        // Tell our children they need to recompute their state.
//        for (auto& childIt : children)
//            childIt.second->setRecomputeExclusionState();
//    }
//
//    // Apply new pending state.
//    mWaitingForIgnoreFileLoad = pending;
//}

void LocalNode::setRecomputeExclusionState(bool includingThisOne)
{
    if (includingThisOne)
    {
        mExclusionState = ES_UNKNOWN;
    }

    if (type == FILENODE)
        return;

    list<LocalNode*> pending(1, this);

    while (!pending.empty())
    {
        auto& node = *pending.front();

        for (auto& childIt : node.children)
        {
            auto& child = *childIt.second;

            if (child.mExclusionState == ES_UNKNOWN)
                continue;

            child.mExclusionState = ES_UNKNOWN;

            if (child.type == FOLDERNODE)
                pending.emplace_back(&child);
        }

        pending.pop_front();
    }
}

bool LocalNode::waitingForIgnoreFileLoad() const
{
    for (auto* node = this; node; node = node->parent)
    {
        if (node->mWaitingForIgnoreFileLoad)
            return true;
    }

    return false;
}

// Query whether a file is excluded by this node or one of its parents.
template<typename PathType>
typename std::enable_if<IsPath<PathType>::value, ExclusionState>::type
LocalNode::exclusionState(const PathType& path, nodetype_t type, m_off_t size) const
{
    // This specialization is only meaningful for directories.
    assert(this->type == FOLDERNODE);

    // We can't determine our child's exclusion state if we don't know our own.
    // Our children are excluded if we are.
    if (mExclusionState != ES_INCLUDED)
        return mExclusionState;

    // Children of unknown type still have to be handled.
    // Scan-blocked appear as TYPE_UNKNOWN and the user must be
	// able to exclude them when they are notified of them

    // Ignore files are only excluded if one of their parents is.
    if (type == FILENODE && path == IGNORE_FILE_NAME)
        return ES_INCLUDED;

    // We can't know the child's state unless our filters are current.
    if (mWaitingForIgnoreFileLoad)
        return ES_UNKNOWN;

    // Computed cloud name and relative cloud path.
    RemotePathPair namePath;

    // Current path component.
    PathType component;

    // Check if any intermediary path components are excluded.
    for (size_t index = 0; path.nextPathComponent(index, component); )
    {
        // Compute cloud name.
        namePath.first = component.toName(*sync->syncs.fsaccess);

        // Compute relative cloud path.
        namePath.second.appendWithSeparator(namePath.first, false);

        // Have we hit the final path component?
        if (!path.hasNextPathComponent(index))
            break;

        // Is this path component excluded?
        if (isExcluded(namePath, FOLDERNODE, false))
            return ES_EXCLUDED;
    }

    // Which node we should start our search from.
    auto* node = this;

    // Does the final path component represent a file?
    if (type == FILENODE)
    {
        // Ignore files are only exluded if one of their parents is.
        if (namePath.first == IGNORE_FILE_NAME)
            return ES_INCLUDED;

        // Is the file excluded by any size filters?
        if (node->isExcluded(namePath, size))
            return ES_EXCLUDED;
    }

    // Is the file excluded by any name filters?
    if (node->isExcluded(namePath, type, node != this))
        return ES_EXCLUDED;

    // File's included.
    return ES_INCLUDED;
}

// Make sure we instantiate the two types.  Jenkins gcc can't handle this in the header.
template ExclusionState LocalNode::exclusionState(const LocalPath& path, nodetype_t type, m_off_t size) const;
template ExclusionState LocalNode::exclusionState(const RemotePath& path, nodetype_t type, m_off_t size) const;

ExclusionState LocalNode::exclusionState(const string& name, nodetype_t type, m_off_t size) const
{
    assert(this->type == FOLDERNODE);

    // Consider providing a specialized implementation to avoid conversion.
    auto fsAccess = sync->syncs.fsaccess.get();
    auto fsType = sync->mFilesystemType;
    auto localname = LocalPath::fromRelativeName(name, *fsAccess, fsType);

    return exclusionState(localname, type, size);
}

ExclusionState LocalNode::exclusionState() const
{
    return mExclusionState;
}

bool LocalNode::isIgnoreFile() const
{
    return mIsIgnoreFile;
}

bool LocalNode::recomputeExclusionState()
{
    // We should never be asked to recompute the root's exclusion state.
    assert(parent);

    // Only recompute the state if it's necessary.
    if (mExclusionState != ES_UNKNOWN)
        return false;

    mExclusionState = parent->exclusionState(localname, type);

    return mExclusionState != ES_UNKNOWN;
}


void LocalNode::ignoreFilterPresenceChanged(bool present, FSNode* fsNode)
{
    // ignore file appeared or disappeared
    if (present)
    {
        // if the file is actually present locally, it'll be loaded after its syncItem()
        filterChain().invalidate();
    }
    else
    {
        rare().filterChain.reset();
    }
    setRecomputeExclusionState(false);
}

#endif // ENABLE_SYNC

void NodeCounter::operator += (const NodeCounter& o)
{
    storage += o.storage;
    files += o.files;
    folders += o.folders;
    versions += o.versions;
    versionStorage += o.versionStorage;
}

void NodeCounter::operator -= (const NodeCounter& o)
{
    storage -= o.storage;
    files -= o.files;
    folders -= o.folders;
    versions -= o.versions;
    versionStorage -= o.versionStorage;
}

std::string NodeCounter::serialize() const
{
    std::string nodeCountersBlob;
    CacheableWriter w(nodeCountersBlob);
    w.serializeu32(uint32_t(files));
    w.serializeu32(uint32_t(folders));
    w.serializei64(storage);
    w.serializeu32(uint32_t(versions));
    w.serializei64(versionStorage);

    return nodeCountersBlob;
}

NodeCounter::NodeCounter(const std::string &blob)
{
    CacheableReader r(blob);
    uint32_t temp;
    r.unserializeu32(temp); files = temp;
    r.unserializeu32(temp); folders = temp;
    r.unserializei64(storage);
    r.unserializeu32(temp); versions = temp;
    r.unserializei64(versionStorage);
}

CloudNode::CloudNode(const Node& n)
    : name(n.hasName() ? n.displayname() : "")
    , type(n.type)
    , handle(n.nodeHandle())
    , parentHandle(n.parent ? n.parent->nodeHandle() : NodeHandle())
    , parentType(n.parent ? n.parent->type : TYPE_UNKNOWN)
    , fingerprint(n.fingerprint())
{
    assert(fingerprint.isvalid || type != FILENODE);
}

bool CloudNode::isIgnoreFile() const
{
    return type == FILENODE && name == IGNORE_FILE_NAME;
}

} // namespace

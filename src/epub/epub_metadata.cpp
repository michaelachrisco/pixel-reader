#include "./epub_metadata.h"
#include "./libxml_iter.h"
#include "sys/filesystem_path.h"

#include <iostream>
#include <cstring>
#include <libxml/parser.h>

std::string epub_get_rootfile_path(const char *container_xml)
{
    xmlDocPtr container_doc = xmlReadMemory(container_xml, strlen(container_xml), nullptr, nullptr, 0);
    if (container_doc == nullptr)
    {
        std::cerr << "Unable to parse container xml" << std::endl;
        return {};
    }

    xmlNodePtr node = xmlDocGetRootElement(container_doc);
    node = elem_first_child(elem_first_by_name(node, BAD_CAST "container"));
    node = elem_first_child(elem_first_by_name(node, BAD_CAST "rootfiles"));
    node = elem_first_by_name(node, BAD_CAST "rootfile");

    std::string rootfile_path;
    if (node)
    {
        auto full_path = xmlGetProp(node, BAD_CAST "full-path");
        auto media_type = xmlGetProp(node, BAD_CAST "media-type");

        if (xmlStrEqual(media_type, BAD_CAST "application/oebps-package+xml"))
        {
            rootfile_path = std::string((const char*) full_path);
        }
        else
        {
            std::cerr << "Found unsupported docroot media type: " << media_type << std::endl;
        }
    }
    else
    {
        std::cerr << "Unable to find rootfile element" << std::endl;
    }

    xmlFreeDoc(container_doc);
    
    return rootfile_path;
}

static std::unordered_map<std::string, ManifestItem> _parse_package_manifest(std::string base_href, xmlNodePtr node)
{
    std::unordered_map<std::string, ManifestItem> manifest;

    node = elem_first_child(elem_first_by_name(node, BAD_CAST "package"));
    node = elem_first_child(elem_first_by_name(node, BAD_CAST "manifest"));
    node = elem_first_by_name(node, BAD_CAST "item");

    while (node)
    {
        const xmlChar *id = xmlGetProp(node, BAD_CAST "id");
        const xmlChar *href = xmlGetProp(node, BAD_CAST "href");
        const xmlChar *media_type = xmlGetProp(node, BAD_CAST "media-type");
        if (id && href && media_type)
        {
            manifest.emplace(
                std::string((const char*) id),
                ManifestItem{
                    fs_path_join(base_href, (const char*) href),
                    std::string((const char*) media_type)
                }
            );
        }
        node = elem_next_by_name(node, BAD_CAST "item");
    }

    return manifest;
}

static std::vector<std::string> _parse_package_spine(xmlNodePtr node)
{
    std::vector<std::string> spine_ids;

    node = elem_first_child(elem_first_by_name(node, BAD_CAST "package"));
    node = elem_first_child(elem_first_by_name(node, BAD_CAST "spine"));
    node = elem_first_by_name(node, BAD_CAST "itemref");

    while (node)
    {
        const xmlChar *idref = xmlGetProp(node, BAD_CAST "idref");
        if (idref)
        {
            spine_ids.emplace_back((const char*)idref);
        }

        node = elem_next_by_name(node, BAD_CAST "itemref");
    }

    return spine_ids;
}

bool epub_get_package_contents(std::string rootfile_path, const char *package_xml, PackageContents &out_package)
{
    xmlDocPtr package_doc = xmlReadMemory(package_xml, strlen(package_xml), nullptr, nullptr, 0);
    if (package_doc == nullptr)
    {
        std::cerr << "Unable to parse package doc" << std::endl;
        return false;
    }

    xmlNodePtr node = xmlDocGetRootElement(package_doc);
    std::string base_href = fs_path_split_dir(rootfile_path).first;

    out_package.id_to_manifest_item = _parse_package_manifest(base_href, node);
    out_package.spine_ids = _parse_package_spine(node);

    xmlFreeDoc(package_doc);

    return true;
}